// Lumen_FarFieldMerge.comp.slang — 把远场光追的命中**合并进**探针光线结果（步骤 26）
//
// 【做什么】步骤 21 用 SDF 求交，步骤 26 用硬件光追打同一条光线。本 pass 按阈值把两者合并：
//   · SDF 命中且 `t_sdf < 阈值` ⇒ 保留 SDF 的结果（近场，交给 SDF）；
//   · 否则（SDF 未命中，或命中的是"远场"） ⇒ 采用光追的命中点与命中距离。
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
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> u_RayHitPos;   // 每光线命中点（步骤 22 的输入）

[[vk::push_constant]] cbuffer MergePC {
    uint4 dims;   // x = 光线数, y = 远场阈值（世界单位，0 = 全部采用光追）, zw 未用
};

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= dims.x) return;
    const float4 rt = u_RtResult[tid.x * 2u];
    if (rt.y < 0.5) return;                      // 光追也没命中 ⇒ 保持 SDF 的结论
    const float4 sdf = u_SdfResult[tid.x];
    const bool sdfNear = (sdf.y > 0.5) && (sdf.x < (float)dims.y);
    if (sdfNear) return;                         // 近场交给 SDF

    const float4 hp = u_RtResult[tid.x * 2u + 1u];
    u_SdfResult[tid.x]  = float4(rt.x, 1.0, sdf.z, 0.0);        // 命中距离与命中标志改成光追的
    u_RayHitPos[tid.x]  = float4(hp.xyz, 1.0);                  // 命中点改成光追的
}
