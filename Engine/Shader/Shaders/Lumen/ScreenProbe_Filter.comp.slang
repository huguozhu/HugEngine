// ScreenProbe_Filter.comp.slang — Screen Probe 的辐射度滤波（步骤 35 / §10 的统一降噪框架）
//
// 【为什么探针需要降噪】每个探针只有 `traceRep`（8~16）条半球光线，且方向是**均匀半球**采样
// （步骤 23 为了白炉可解析而选的），于是单探针的 SH 估计方差很大；步骤 24 把它沿像素法线重建成
// 逐像素辐照度后，噪声直接落在画面上。L6 的第一件事就是给它加"空间 + 时域"两级滤波。
//
// 【空间级：3×3 单元 + YCoCg AABB】以**单元**（16×16 像素）为步长取 3×3 邻域（经单元→探针映射），
// 统计邻域在每个 SH 系数上的均值 μ 与标准差 σ，构造 AABB = [μ - γσ, μ + γσ]，再把每个邻居的值
// **截断**到该区间后求平均。为什么在 YCoCg 里做：Y 是亮度、Co/Cg 是色度；在 RGB 里逐通道截断会让
// 色度噪声被亮度噪声掩盖而漏过去。这一步同时降低方差（平均）并抑制离群（截断）。
//
// 【时域级：重投影 + EMA】探针是世界空间点：把它用**上一帧**的 viewProj 投回上一帧屏幕 → 上一帧的
// 单元 → 上一帧的探针；位置与法线都吻合才认为"这是同一处的历史"。历史值也截断到同一个 AABB 后做
// EMA：out = lerp(spatial, clamp(hist), α)。截断历史是必须的：历史携带上一帧的估计，不截断的话
// 相机一动、AABB 变了，旧值会被"拖"进来（拖影）。
//
// 【白炉为什么仍然精确】白炉下所有探针的 SH 几乎相同（√π 量级）⇒ σ ≈ 0 ⇒ AABB 退化成一个点 ⇒
// 截断与平均都是恒等；时域 EMA 两端也相同 ⇒ 恒等。故本 pass 不改白炉标度（步骤 23/38 的读数）。
//
// 【三种模式】由 dims.w 给出，供**同一构建内**做 A/B（§11.4 的验收口径要求同一把尺子）：
//   0 = 直通（输出与输入逐位相同，等价于"没有这一步"）
//   1 = 仅空间
//   2 = 空间 + 时域（默认）
//   3 = 空间 + 时域 + AABB 截断后的亮度均值保持（截断对偏斜分布有系统性下移，见下面的说明）
// 统计缓冲**同时**累计"输入"与"输出"两组一阶/二阶矩，所以噪声下降能在**同一次运行**里读出来，
// 不必跨运行对照（跨运行对照会被既有的运行间抖动污染）。
//
// 【统计槽位】（u_Stats，u32）
//   0 = 有效探针数        1 = Σ|in| ×4096        2 = Σin² ×4096
//   3 = Σ|out| ×4096      4 = Σout² ×4096        5 = Σ|out − hist| ×4096（帧间变化，有历史才算）
//   6 = 采纳历史的探针数   7 = 邻域有效样本总数     8 = 有历史候选但未过门限的次数
//   9 = 被 AABB 截断的通道数 10 = 直通模式下的探针数
#include "Lumen/ScreenProbe.slang"

// 历史条目：过滤结果的最小镜像。位置/法线用于重投影校验，SH 用于时域混合。
// 80 B（5 × 16 B）——比探针本体（96 B）小，且不含 uv/meta（下游用不到）。
struct ProbeHistory {
    float4 position;   // xyz = 世界坐标（过滤时探针的世界位置）, w = 有效性（> 0.5 为有效）
    float4 normal;     // xyz = 世界法线, w = 保留
    float4 shR;        // 4 系数（与 ScreenProbe 同布局）
    float4 shG;
    float4 shB;
};

[[vk::binding(0, 0)]] StructuredBuffer<ScreenProbe>    u_Probes;      // 当帧探针（布置 + SH 投影已写）
[[vk::binding(1, 0)]] RWStructuredBuffer<ScreenProbe>  u_Filtered;    // 过滤结果（下游：逐像素辐照度 / DDGI 输入）
[[vk::binding(2, 0)]] StructuredBuffer<uint>           u_CellProbe;   // 当帧：单元 → 探针下标（0xFFFFFFFF = 无）
[[vk::binding(3, 0)]] StructuredBuffer<uint>           u_CellHist;    // 上一帧：单元 → 探针下标
[[vk::binding(4, 0)]] RWStructuredBuffer<uint>         u_CellHistOut; // 写出当帧映射（下一帧的历史）
[[vk::binding(5, 0)]] StructuredBuffer<ProbeHistory>   u_Hist;        // 上一帧的过滤结果
[[vk::binding(6, 0)]] RWStructuredBuffer<ProbeHistory> u_HistOut;     // 本帧过滤结果（下一帧的历史）
[[vk::binding(7, 0)]] RWStructuredBuffer<uint>         u_Stats;       // 槽位见文件头

[[vk::push_constant]] cbuffer FilterPC {
    float4x4 prevViewProj;   // 上一帧的 viewProj（重投影用；本帧的由 CPU 侧保存后下帧传入）
    uint4    dims;           // x = 探针数, y = cellsX, z = cellsY, w = 模式（0 直通 / 1 空间 / 2 空间+时域）
    float4   params;         // x = AABB 的 γ, y = 时域历史权重 α, z = 历史可用（1/0）, w = 重投影相对容差
};

// YCoCg：Y = 亮度，Co/Cg = 两个色度轴。截断在**这个空间**里做，色度噪声才不会被亮度掩盖。
float3 RgbToYCoCg(float3 c) {
    return float3(0.25 * c.r + 0.50 * c.g + 0.25 * c.b,
                  0.50 * c.r - 0.50 * c.b,
                 -0.25 * c.r + 0.50 * c.g - 0.25 * c.b);
}
float3 YCoCgToRgb(float3 c) {
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    const uint i        = tid.x;
    const uint cellsX   = dims.y;
    const uint cellsY   = dims.z;
    const uint cellCnt  = cellsX * cellsY;
    const uint mode     = dims.w;

    // ① 单元映射的"搬运"：线程数取 max(探针数, 单元数)，每个单元一个线程把当帧映射抄进历史缓冲。
    // 为什么不按探针写：一个合并探针覆盖 2×2 个单元，逐个探针写会漏掉 3 个单元，那些单元下一帧
    // 就会拿到**两帧前**的旧下标（位置校验多半能挡住，但那是"靠容错"，不是"对"）。
    if (i < cellCnt) u_CellHistOut[i] = u_CellProbe[i];
    if (i >= dims.x) return;

    ScreenProbe p = u_Probes[i];
    ScreenProbe outc = p;   // 直通字段（位置/法线/uv/meta）保持不变，只替换 SH

    ProbeHistory h;
    h.position = p.position;
    h.normal   = float4(normalize(p.normal.xyz), 0.0);
    h.shR = p.shR; h.shG = p.shG; h.shB = p.shB;

    if (p.position.w < 0.5) {   // 无几何：SH 清零（与 SH 投影 pass 的约定一致），历史标无效
        outc.shR = float4(0.0); outc.shG = float4(0.0); outc.shB = float4(0.0);
        u_Filtered[i] = outc;
        h.position.w = 0.0;
        h.shR = float4(0.0); h.shG = float4(0.0); h.shB = float4(0.0);
        u_HistOut[i] = h;
        return;
    }

    const float4 inR = p.shR, inG = p.shG, inB = p.shB;
    const float  inY0 = 0.25 * inR.x + 0.50 * inG.x + 0.25 * inB.x;   // l0 的亮度（噪声统计口径）

    if (mode == 0u) {   // 直通：输出与输入逐位相同（A/B 的基线），但仍写历史（两条路只有滤波一处不同）
        outc.shR = inR; outc.shG = inG; outc.shB = inB;
        u_Filtered[i] = outc;
        u_HistOut[i] = h;
        InterlockedAdd(u_Stats[0], 1u);
        InterlockedAdd(u_Stats[1], (uint)(abs(inY0) * 4096.0 + 0.5));
        InterlockedAdd(u_Stats[2], (uint)(inY0 * inY0 * 4096.0 + 0.5));
        InterlockedAdd(u_Stats[3], (uint)(abs(inY0) * 4096.0 + 0.5));
        InterlockedAdd(u_Stats[4], (uint)(inY0 * inY0 * 4096.0 + 0.5));
        InterlockedAdd(u_Stats[10], 1u);
        return;
    }

    // ② 空间级：收集 3×3 单元的邻居探针
    uint nbr[9];
    uint nbrCount = 0u;
    const int2 c0 = int2(p.uv.zw * 2.0);   // tile 坐标 → 单元坐标（左上角；合并探针由邻域补齐）
    for (int dy = -1; dy <= 1; ++dy) {
        const int cy = c0.y + dy;
        if (cy < 0 || cy >= (int)cellsY) continue;
        for (int dx = -1; dx <= 1; ++dx) {
            const int cx = c0.x + dx;
            if (cx < 0 || cx >= (int)cellsX) continue;
            const uint j = u_CellProbe[(uint)cy * cellsX + (uint)cx];
            if (j == 0xFFFFFFFFu || j >= dims.x) continue;
            if (u_Probes[j].position.w < 0.5) continue;
            if (nbrCount < 9u) nbr[nbrCount] = j;
            ++nbrCount;
        }
    }
    if (nbrCount == 0u) { nbr[0] = i; nbrCount = 1u; }   // 映射尚未写出：只用自己，不制造黑
    const uint n = min(nbrCount, 9u);

    // 4 系数 × YCoCg：均值/标准差（构造 AABB）与逐邻居缓存（同一次遍历即可复用）
    float3 meanY[4], stdY[4];
    float3 nbrY[9][4];
    for (uint k = 0u; k < 4u; ++k) {
        float3 acc = float3(0.0), acc2 = float3(0.0);
        for (uint s = 0u; s < n; ++s) {
            const uint j = nbr[s];
            const ScreenProbe q = u_Probes[j];
            const float3 ycc = RgbToYCoCg(float3(q.shR[k], q.shG[k], q.shB[k]));
            nbrY[s][k] = ycc;
            acc  += ycc;
            acc2 += ycc * ycc;
        }
        const float3 mu = acc / (float)n;
        meanY[k] = mu;
        stdY[k]  = sqrt(max(acc2 / (float)n - mu * mu, float3(0.0)));
    }
    const float gamma = params.x;

    // 截断 + 平均（模式 3 额外做"亮度均值保持"，见下面的 rescale）
    const bool keepMean = (mode == 3u);
    float3 spatialY[4];
    for (uint k = 0u; k < 4u; ++k) {
        const float3 lo = meanY[k] - gamma * stdY[k];
        const float3 hi = meanY[k] + gamma * stdY[k];
        float3 acc = float3(0.0);
        float  ySumRaw = 0.0, ySumClamped = 0.0;
        for (uint s = 0u; s < n; ++s) {
            const float3 v  = nbrY[s][k];
            const float3 cl = clamp(v, lo, hi);
            if (any(cl != v)) InterlockedAdd(u_Stats[9], 1u);
            acc += cl;
            ySumRaw     += v.x;
            ySumClamped += cl.x;
        }
        float3 res = acc / (float)n;
        // 【模式 3】AABB 截断对**偏斜**分布不是无偏的（长尾被削掉 ⇒ 均值下移）。
        // 用"截断前/后 的 Y 之和"的比把亮度均值补回来（ReSTIR 的 bias correction 同思路：
        // 只补亮度、不补色度 —— 色度的离群本来就该被压掉）。模式 2 不做这一步。
        if (keepMean && abs(ySumClamped) > 1e-6) {
            const float scale = ySumRaw / ySumClamped;
            res *= clamp(scale, 0.5, 2.0);   // 限幅：极端情况下不让 corrections 自己变成噪声源
        }
        spatialY[k] = res;
    }
    InterlockedAdd(u_Stats[7], n);

    // ③ 时域级：重投影到上一帧的单元 → 上一帧的探针
    float3 resultY[4];
    bool   usedHistory = false;
    float  histY0 = 0.0;
    if (mode >= 2u && params.z > 0.5) {        const float4 clip = mul(prevViewProj, float4(p.position.xyz, 1.0));
        if (clip.w > 0.0) {
            const float2 ndc = clip.xy / clip.w;
            const float2 uv  = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
            if (uv.x >= 0.0 && uv.x < 1.0 && uv.y >= 0.0 && uv.y < 1.0) {
                const uint2 pc = uint2(uv * float2((float)cellsX, (float)cellsY));
                const uint  pj = u_CellHist[min(pc.y, cellsY - 1u) * cellsX + min(pc.x, cellsX - 1u)];
                if (pj != 0xFFFFFFFFu && pj < dims.x) {
                    const ProbeHistory hist = u_Hist[pj];
                    const float dist = length(hist.position.xyz - p.position.xyz);
                    const float tol  = params.w * max(clip.w, 1.0);   // 容差随视图深度缩放
                    const float ndot = dot(h.normal.xyz, normalize(hist.normal.xyz));
                    if (hist.position.w > 0.5 && dist <= tol && ndot >= 0.9) {
                        const float a = clamp(params.y, 0.0, 0.98);
                        for (uint k = 0u; k < 4u; ++k) {
                            const float3 hv = RgbToYCoCg(float3(hist.shR[k], hist.shG[k], hist.shB[k]));
                            const float3 lo = meanY[k] - gamma * stdY[k];
                            const float3 hi = meanY[k] + gamma * stdY[k];
                            resultY[k] = lerp(spatialY[k], clamp(hv, lo, hi), a);
                        }
                        histY0 = 0.25 * hist.shR.x + 0.50 * hist.shG.x + 0.25 * hist.shB.x;
                        usedHistory = true;
                        InterlockedAdd(u_Stats[6], 1u);
                    } else {
                        InterlockedAdd(u_Stats[8], 1u);   // 有候选但没通过门限（差分用）
                    }
                }
            }
        }
    }
    if (!usedHistory) for (uint k = 0u; k < 4u; ++k) resultY[k] = spatialY[k];

    // ④ 写回（YCoCg → RGB；每个系数独立取回三通道）
    float3 c[4];
    for (uint k = 0u; k < 4u; ++k) c[k] = YCoCgToRgb(resultY[k]);
    outc.shR = float4(c[0].r, c[1].r, c[2].r, c[3].r);
    outc.shG = float4(c[0].g, c[1].g, c[2].g, c[3].g);
    outc.shB = float4(c[0].b, c[1].b, c[2].b, c[3].b);
    u_Filtered[i] = outc;

    h.shR = outc.shR; h.shG = outc.shG; h.shB = outc.shB;
    u_HistOut[i] = h;

    // ⑤ 统计：输入 / 输出两组一阶二阶矩（同一帧内即可读出噪声下降）
    const float outY0 = 0.25 * outc.shR.x + 0.50 * outc.shG.x + 0.25 * outc.shB.x;
    InterlockedAdd(u_Stats[0], 1u);
    InterlockedAdd(u_Stats[1], (uint)(abs(inY0) * 4096.0 + 0.5));
    InterlockedAdd(u_Stats[2], (uint)(inY0 * inY0 * 4096.0 + 0.5));
    InterlockedAdd(u_Stats[3], (uint)(abs(outY0) * 4096.0 + 0.5));
    InterlockedAdd(u_Stats[4], (uint)(outY0 * outY0 * 4096.0 + 0.5));
    if (usedHistory) InterlockedAdd(u_Stats[5], (uint)(abs(outY0 - histY0) * 4096.0 + 0.5));
}
