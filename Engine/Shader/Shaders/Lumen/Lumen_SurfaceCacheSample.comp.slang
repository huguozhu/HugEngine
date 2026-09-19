// Lumen_SurfaceCacheSample.comp.slang — 命中点着色：从 Surface Cache atlas 取材质（步骤 22）
//
// 【做什么】对步骤 21 的每条**命中**光线：
//   1. 用命中点找它属于哪张卡（"包含它的最小 AABB"）；卡 → 逻辑页 → 页表里的物理页；
//   2. 页状态必须是 `Captured`/`Dirty`（有内容）才算"页命中"；否则返回**中性值 + 缺页标记**
//      （Lumen 的做法：缺页不能返回黑，那会在画面上留黑斑；中性灰 + 标记，交给反馈去补页）；
//   3. 由命中点在卡平面上的 2D 坐标换算到 atlas texel（**页边界**夹取，并计数）；
//   4. 同时采一次同像素的 **GBuffer albedo** 写到另一个缓冲，供 CPU 做"同一几何上材质是否一致"的对照。//
// 【为什么这一步能把 L2 和 L3 连起来】命中点材质不再来自屏幕（GBuffer），而是来自 atlas ——
// 这正是"Surface Cache 可被后续阶段当作命中点材质源"（步骤 19 的判据）兑现的地方。
#include "Lumen/SurfaceCache.slang"

struct ShadeCard {          // 与 C++ 的 ShadeCard 逐字段一致（32 B）
    float4 aabbLo;          // xyz = mesh AABB 最小角, w = 边长
    uint4  axisDirPageCard; // x = 轴(0/1/2), y = 方向(±1), z = 逻辑页, w = 卡号
};

[[vk::binding(0, 0)]] Texture2D<float4>                        u_AtlasAlbedo;
[[vk::binding(1, 0)]] StructuredBuffer<ShadeCard>              u_Cards;
[[vk::binding(2, 0)]] StructuredBuffer<SurfaceCachePageEntry>  u_Pages;
[[vk::binding(3, 0)]] StructuredBuffer<float4>                 u_RayHitPos;   // xyz = 命中点, w = 是否命中
[[vk::binding(4, 0)]] Texture2D<float4>                        u_GBufferAlbedo;
[[vk::binding(5, 0)]] RWStructuredBuffer<float4>               u_Out;         // xyz = atlas albedo, w = 页命中
[[vk::binding(6, 0)]] RWStructuredBuffer<float4>               u_OutGb;       // xyz = 同像素 GBuffer albedo, w = 有效
[[vk::binding(7, 0)]] RWStructuredBuffer<uint>                 u_Stats;       // 0=页命中 1=不在任何卡内 2=卡在但页无内容 3=总计 4=越界夹取 5=多重覆盖命中 6=其中页命中
[[vk::binding(8, 0)]] Texture2D<float4>                        u_GBufferWorldPos;  // 用于判定"命中点是否就是该像素的前景表面"
[[vk::binding(9, 0)]] RWStructuredBuffer<float4>               u_OutBest;     // xyz = 多卡覆盖下最贴合 GBuffer 的候选 albedo, w = 有效（归因实验用）

[[vk::push_constant]] cbuffer ShadePC {
    uint4  dims;         // x = 总光线数, y = 卡数, z = 屏宽, w = 屏高
    float4 vp0;          // 视图投影 4 行（显式传，避免 mat4 约定问题）
    float4 vp1;
    float4 vp2;
    float4 vp3;
    float4 atlasParams;  // x = 目标页边长(texel), y = atlas 边长(texel), z = 中性值, w = 未用
};

float4 WorldToClip(float3 p) {
    return float4(dot(vp0.xyz, p) + vp0.w, dot(vp1.xyz, p) + vp1.w,
                  dot(vp2.xyz, p) + vp2.w, dot(vp3.xyz, p) + vp3.w);
}

// 从 VP 反求相机位置（标准透视：P 的第 0/1 行只有对角元、第 3 行为 (0,0,-1,0)）：
//   VP 第 i 行 .xyz = P_ii · V 第 i 行 .xyz（i=0,1），VP 第 3 行 .xyz = -V 第 2 行 .xyz
//   ⇒ 旋转 R 的三行 = normalize(vp0.xyz) / normalize(vp1.xyz) / (-vp3.xyz)
//   ⇒ 平移 t = (vp0.w/|vp0.xyz|, vp1.w/|vp1.xyz|, -vp3.w)，相机位置 = -Rᵀ·t
// 用途：对照判定需要一个**随距离缩放**的容差（本场景尺度上千，固定容差会把不同表面判成同一表面）。
float3 CameraPos() {
    const float3 r0 = normalize(vp0.xyz);
    const float3 r1 = normalize(vp1.xyz);
    const float3 r2 = -normalize(vp3.xyz);
    const float  t0 = vp0.w / length(vp0.xyz);
    const float  t1 = vp1.w / length(vp1.xyz);
    const float  t2 = -vp3.w;
    return -float3(dot(r0, float3(t0, t1, t2)), dot(r1, float3(t0, t1, t2)), dot(r2, float3(t0, t1, t2)));
}

// 由"卡 + 命中点"取 atlas 中的 albedo。返回 w = 1 表示拿到了内容，0 表示该页没有内容。
// 页内 texel 由命中点在卡平面上的 2D 坐标换算，**页边界**做夹取（并计数）。
float4 SampleCardAlbedo(ShadeCard card, float3 p, inout uint clampCount) {
    const uint page = card.axisDirPageCard.z;
    if (page == kSCInvalidPage) return float4(0.0, 0.0, 0.0, 0.0);
    const SurfaceCachePageEntry e = u_Pages[page];
    const bool hasContent = (e.state == kSCPageState_Captured || e.state == kSCPageState_Dirty) &&
                            (e.pageIndex != kSCInvalidPage);
    if (!hasContent) return float4(0.0, 0.0, 0.0, 0.0);

    const uint a = card.axisDirPageCard.x;
    const uint b = (a + 1u) % 3u;
    const uint c = (a + 2u) % 3u;
    const float3 cardLo = card.aabbLo.xyz;   // 先落到局部变量，Slang 不允许对分量取地址
    const float3 local  = p - cardLo;
    const float2 uvInCard = float2(local[b], local[c]) / card.aabbLo.w;
    int2 texel = int2(uvInCard * atlasParams.x);
    const int2 before = texel;
    texel = clamp(texel, int2(0, 0), int2((int)atlasParams.x - 1, (int)atlasParams.x - 1));
    if (any(texel != before)) clampCount += 1u;

    const uint pagesPerRow = (uint)(atlasParams.y / atlasParams.x);
    const uint2 pageOrigin = uint2((e.pageIndex % pagesPerRow) * (uint)atlasParams.x,
                                   (e.pageIndex / pagesPerRow) * (uint)atlasParams.x);
    const float4 albedo = u_AtlasAlbedo.Load(int3((int2)(pageOrigin + uint2(texel)), 0));
    // atlas 的 alpha 是捕获时写的"该 texel 有内容"标志（见 SurfaceCache_Capture.comp.slang）
    return float4(albedo.rgb, albedo.a > 0.5 ? 1.0 : 0.0);
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= dims.x) return;
    u_OutBest[tid.x] = float4(0.0, 0.0, 0.0, 0.0);
    const float4 hp = u_RayHitPos[tid.x];
    if (hp.w < 0.5) return;                                   // 这条光线没命中
    InterlockedAdd(u_Stats[3], 1u);

    const float3 p = hp.xyz;
    const float  neutral = atlasParams.z;

    // ── ① 对照基准：同像素的 GBuffer albedo —— 只有命中点确实就是该像素的**前景表面**时才有意义 ──
    // 探针光线打的是**二次命中**，重投影到的屏幕像素上通常是另一个（更近的）物体，直接比较会把
    // "几何不同"算成"材质不同"（实测 12362 个样本平均差 0.34，全是这种噪声）。故用 GBuffer 世界坐标
    // 做一致性判定，容差随视距缩放（场景尺度 2730，固定容差会把相距十几单位的不同表面判成同一表面）。
    float3 gbAlbedo = float3(0.0, 0.0, 0.0);
    bool   gbValid  = false;
    const float4 clip = WorldToClip(p);
    if (clip.w > 0.0) {
        const float2 ndc = clip.xy / clip.w;
        const float2 uv  = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0) {
            const uint2 px = uint2((uint)clamp(uv.x * (float)dims.z, 0.0, (float)dims.z - 1.0),
                                   (uint)clamp(uv.y * (float)dims.w, 0.0, (float)dims.w - 1.0));
            const float4 wp = u_GBufferWorldPos.Load(int3((int2)px, 0));
            const float tol = max(0.05, 0.003 * distance(CameraPos(), p));
            if (all(abs(wp.xyz) > 1e-4) && distance(wp.xyz, p) <= tol) {
                gbAlbedo = u_GBufferAlbedo.Load(int3((int2)px, 0)).rgb;
                gbValid  = true;
            }
        }
    }
    u_OutGb[tid.x] = float4(gbAlbedo, gbValid ? 1.0 : 0.0);

    // ── ② 遍历所有"包含 p 的卡" ──
    // 卡片用**立方 AABB**（边长取 mesh 包围盒最大轴），互相重叠很常见：实测页命中的光线里
    // **100%** 都落在 ≥2 张卡的 AABB 内。所以"选哪张卡"本身就是一个误差源，必须与"atlas 内容是否
    // 正确"分开度量 —— 于是这里**同时**记录两个候选：
    //   chosen = 最小 AABB（当前策略，也是 Lumen 的策略）；best = 与 GBuffer 材质最接近的候选（下界）。
    // 两者与 GBuffer 的差一起回报给 CPU：若 best 明显小于 chosen，误差来自选卡；若两者相近，误差
    // 来自 atlas 内容 / texel 分辨率。
    uint  cover = 0, pageHitCards = 0, clampCount = 0;
    uint  bestCard = 0xFFFFFFFFu, bestFitCard = 0xFFFFFFFFu;
    float bestSide = 1e30, bestFitDiff = 1e30;
    float3 bestFitAlbedo = float3(0.0, 0.0, 0.0);
    bool   chosenHasContent = false;
    float3 chosenAlbedo = float3(0.0, 0.0, 0.0);

    for (uint i = 0; i < dims.y; ++i) {
        const float3 lo = u_Cards[i].aabbLo.xyz;
        const float3 hi = lo + u_Cards[i].aabbLo.w;
        if (any(p < lo) || any(p > hi)) continue;
        ++cover;
        // 最小 AABB 优先（当前策略）
        const bool isChosen = (u_Cards[i].aabbLo.w < bestSide);
        if (isChosen) { bestSide = u_Cards[i].aabbLo.w; bestCard = i; }

        const float4 cand = SampleCardAlbedo(u_Cards[i], p, clampCount);
        if (cand.w < 0.5) continue;
        ++pageHitCards;
        if (gbValid) {
            const float d = length(cand.rgb - gbAlbedo);
            if (d < bestFitDiff) { bestFitDiff = d; bestFitCard = i; bestFitAlbedo = cand.rgb; }
        }
    }

    if (bestCard != 0xFFFFFFFFu) {
        const float4 chosen = SampleCardAlbedo(u_Cards[bestCard], p, clampCount);
        chosenHasContent = chosen.w > 0.5;
        chosenAlbedo = chosen.rgb;
    }

    if (cover > 1u) InterlockedAdd(u_Stats[5], 1u);
    if (bestCard == 0xFFFFFFFFu)      InterlockedAdd(u_Stats[1], 1u);   // 不在任何卡的 AABB 内
    else if (!chosenHasContent)       InterlockedAdd(u_Stats[2], 1u);   // 卡在但页无内容
    else                            { InterlockedAdd(u_Stats[0], 1u); if (cover > 1u) InterlockedAdd(u_Stats[6], 1u); }
    if (clampCount) InterlockedAdd(u_Stats[4], clampCount);

    // ── ③ 输出 ──
    // u_Out.w：0 = 缺页（返回中性值，不能返回黑，否则画面上留黑斑）；1 = 页命中且单卡覆盖；2 = 页命中但多卡覆盖
    u_Out[tid.x] = chosenHasContent
        ? float4(chosenAlbedo, cover > 1u ? 2.0 : 1.0)
        : float4(neutral, neutral, neutral, 0.0);
    // u_OutBest：多卡覆盖下"最贴合 GBuffer 材质"的候选（仅用于归因实验）
    if (gbValid && bestFitCard != 0xFFFFFFFFu) u_OutBest[tid.x] = float4(bestFitAlbedo, 1.0);
}
