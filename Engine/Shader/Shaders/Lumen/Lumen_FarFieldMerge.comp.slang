// Lumen_FarFieldMerge.comp.slang — 把远场光追的命中**合并进**探针光线结果（步骤 26 / 27）
//
// 【做什么】
//   步骤 21 用 SDF 求交、步骤 26 用硬件光追打同一条光线，本 pass 把两者合并：
//     · 近场（SDF 命中且距离 < 阈值下界）⇒ 用 SDF 的命中点；
//     · 远场（SDF 未命中，或超过阈值上界）⇒ 用光追的命中点；
//     · **重叠带 `[t0, t1]` 内**（步骤 27）⇒ 主命中点仍写 SDF 的，同时把光追命中点与
//       **混合权重**写进 fade 缓冲，由着色 pass 对两侧材质做线性混合 ⇒ 阈值处没有阶跃。
//
// 【为什么混合必须发生在"材质"而不是"位置"上】把两个命中点插值出一个中间位置没有物理意义
//（会落在两个表面之间、取到空气的材质）。所以这里只负责**把两个点都交出去**，由
// `Lumen_SurfaceCacheSample` 各取一次材质再按权重混合 —— 混合的是"看到什么"，不是"在哪"。
//
// 【权重曲线】`w = smoothstep(t0, t1, t_sdf)`：w=0 纯 SDF、w=1 纯光追。用 smoothstep 而不是线性，
// 是为了让切换在两端都一阶连续（线性在 t0/t1 处仍有斜率突变）。`t0 == t1` 时退化成硬切换，
// 保留"有/无 fade"的 A/B 对照能力（步骤 27 的验收就是拿这两个配置对比）。
//
// 【为什么必须有这一步】实测（§附三十三）SDF 的 march 在**远场根本不可用**：
// 全球 clipmap 的近层体素就有 21.3 世界单位、远层 42.7，而本场景的墙在几十到几百单位外；
// eps 取 1 个体素时"命中率 94%"全是"离表面一个体素内就判命中"的假命中（命中距离均值 16.4
// 单位，而三角形在 100+ 单位外）；eps 收到 0.1 个体素后命中率只剩 18% —— 也就是说"近场 SDF +
// 远场 HW RT"不是优化，而是让这条链路**成立**的必要条件（正是计划里步骤 26/27 的位置）。
//
// 【为什么不直接让 rgen 改写】验收要"逐光线比较 SDF 与三角形"，比较必须在**改写之前**做
//（`Lumen_FarFieldCompare` 读的就是原始 SDF 结果）。合并单独一步，顺序清晰、也便于回退。
#include "ShaderTypes.slang"
#include "Lumen/LumenFarField.slang"

[[vk::binding(0, 0)]] RWStructuredBuffer<float4> u_SdfResult;   // 就地改写：x=t, y=hit, z=dot(dir,n)
[[vk::binding(1, 0)]] StructuredBuffer<float4>   u_RtResult;    // 每光线 2 条：[2i]=(t,hit,lum), [2i+1]=(命中点,1)
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> u_RayHitPos;   // 命中点（步骤 22 的输入）
// **每光线两个 float4**：[2i] = (光追命中点, 光追权重 w)，[2i+1] = (SDF 距离, SDF 命中, 光追距离, 光追命中)
// 第二个条目是给"按距离分桶"的连续性读数用的：分桶必须按**稳定量**（SDF 距离），
// 否则切换前后分桶依据本身会跳变（SDF 命中距离几十、光追命中距离几百），曲线反而失真。
[[vk::binding(3, 0)]] RWStructuredBuffer<float4> u_Fade;
// 0=纯 SDF 1=纯光追 2=重叠带混合 3=分类不变量违例（近场保留了越界的 SDF 命中）
// 4=分类不变量违例（两侧都命中却按光追判成"纯光追"但 SDF 距离没到上界）5=不变量违例（SDF 未命中却走了光追以外）
[[vk::binding(4, 0)]] RWStructuredBuffer<uint>   u_Stats;

[[vk::push_constant]] cbuffer MergePC {
    uint4 dims;   // x = 光线数, y = 重叠带下界 t0, z = 上界 t1, w = 未用
};

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= dims.x) return;
    u_Fade[tid.x * 2u]      = float4(0.0, 0.0, 0.0, -1.0);   // 默认"没有副点"
    u_Fade[tid.x * 2u + 1u] = float4(0.0, 0.0, 0.0, 0.0);

    const float4 rt = u_RtResult[tid.x * 2u];
    if (rt.y < 0.5) return;                        // 光追也没命中 ⇒ 保持 SDF 的结论

    const float4 sdf   = u_SdfResult[tid.x];
    const float4 rtPos = u_RtResult[tid.x * 2u + 1u];
    const float  t0 = (float)dims.y;
    const float  t1 = (float)dims.z;

    if (sdf.y > 0.5) {
        // 两侧都命中：主命中点保持 SDF，权重决定材质混合比例
        float w = (t1 > t0) ? clamp((sdf.x - t0) / (t1 - t0), 0.0, 1.0) : (sdf.x >= t1 ? 1.0 : 0.0);
        w = w * w * (3.0 - 2.0 * w);               // smoothstep
        u_Fade[tid.x * 2u]      = float4(rtPos.xyz, w);
        u_Fade[tid.x * 2u + 1u] = float4(sdf.x, 1.0, rt.x, 1.0);
        if (w <= 0.0)      InterlockedAdd(u_Stats[0], 1u);
        else if (w >= 1.0) InterlockedAdd(u_Stats[1], 1u);
        else               InterlockedAdd(u_Stats[2], 1u);
        // 【分类不变量自检（步骤 29 的"正确混合"判据之一）】三类各自的判定条件必须成立，
        // 违例计数应当恒为 0：纯 SDF ⇒ 距离在下界内；纯光追（两侧都命中）⇒ 距离到了上界。
        // 这不是"再算一遍"，而是把"混合是否按定义执行"变成可断言的读数。
        if (w <= 0.0 && !(sdf.x < t0))  InterlockedAdd(u_Stats[3], 1u);
        if (w >= 1.0 && !(t1 <= t0 || sdf.x >= t1)) InterlockedAdd(u_Stats[4], 1u);
        if (w >= 1.0) {                            // 权重到 1 ⇒ 主命中点也换成光追的
            u_SdfResult[tid.x] = float4(rt.x, 1.0, sdf.z, 0.0);
            u_RayHitPos[tid.x] = float4(rtPos.xyz, 1.0);
        }
        return;
    }

    // SDF 未命中：整条光线交给光追（近场没有它的信息）
    u_SdfResult[tid.x] = float4(rt.x, 1.0, sdf.z, 0.0);
    u_RayHitPos[tid.x] = float4(rtPos.xyz, 1.0);
    u_Fade[tid.x * 2u + 1u] = float4(0.0, 0.0, rt.x, 1.0);
    InterlockedAdd(u_Stats[1], 1u);
    if (sdf.y > 0.5) InterlockedAdd(u_Stats[5], 1u);   // 该分支只应处理"SDF 未命中"的光线
}
