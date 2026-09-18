// ============================================================
// DecalProject.frag — GBuffer 投影贴花（任务 24）
//
// 【为什么是"投影"而不是"投射片"】旧 MVP 是一张半透明四边形，它只能"贴在"表面上：
// 平的地方凑合能看，曲面/台阶/斜坡上就会穿模、悬空、被深度裁掉。投影贴花改为：
//   1. 画贴花体积盒（顶点着色器）→ 只覆盖"可能被影响"的屏幕区域；
//   2. 片段着色器读 GBuffer 的**世界坐标**（MRT4）拿到该像素上**真实表面**的点；
//   3. 把该点变换到贴花局部空间，落在盒子外的直接 discard；
//   4. 盒内的点按局部 xy 反算 UV，采样贴花纹理。
// 于是贴花自动贴合任何朝向的表面，且被几何体正确遮挡（像素在世界点之外就不会被画到）。
//
// 【为什么用混合而不是直接写】本 Pass 与 GBuffer 共用 8 个 MRT（其余 6 个 writeMask=None），
// 但**不能**把 MRT0/MRT1 当纹理采样（同一附件读写 = feedback loop）。所以最终颜色由
// 硬件混合完成：dst = src * srcAlpha + dst * (1 - srcAlpha)，法线/粗糙度同样按 alpha 混合
//（法线在 GBuffer 里是 N*0.5+0.5 的仿射编码，逐分量线性混合与解码后混合等价）。
//
// 【天空判定】世界坐标对天空像素是清屏值（0,0,0），不能作为"没有表面"的依据，
// 因此用 GBuffer 深度 > 0 判定：depth >= 1 表示该像素没有几何体（天空/背景）。
// ============================================================

#include "common.slang"

struct FSOutput {
    [[vk::location(0)]] float4 albedoMetallic  : SV_Target0;  // rgb = 贴花颜色，a = 不透明度（用于混合）
    [[vk::location(1)]] float4 normalRoughness : SV_Target1;  // xyz = 投影法线（N*0.5+0.5），a = 粗糙度
};

// GBuffer 世界坐标（MRT4）与深度：本 Pass 只把深度当**采样**输入（不作为附件）
[[vk::binding(20, kGPUDescSet_PerFrame)]] Texture2D<float4> u_GBufferWorldPos;
[[vk::binding(21, kGPUDescSet_PerFrame)]] Texture2D<float>  u_GBufferDepth;
[[vk::binding(22, kGPUDescSet_PerFrame)]] SamplerState      u_PointSampler;

[[vk::push_constant]] cbuffer DecalConstants {
    float4x4 viewProj;
    float4   rotRow0;
    float4   rotRow1;
    float4   rotRow2;
    float4   invRotRow0;
    float4   invRotRow1;
    float4   invRotRow2;
    float4   decalOrigin;
    float4   halfExtents;
    float4   colorOpacity;
    float4   normalMetal;
    float4   params;        // x = 粗糙度，y = bindless 纹理句柄，z/w = 1/屏幕宽、1/屏幕高
    uint     flags;
    uint     _pad0;
    uint     _pad1;
    uint     _pad2;
};

FSOutput fragmentMain(float4 position : SV_Position) {
    FSOutput output;

    // 1. 屏幕 UV（push constant 里预先存好 1/宽、1/高，省一次除法）
    float2 uv = position.xy * params.zw;

    // 2. 天空/背景：没有表面可贴（世界坐标此时是清屏值，不可作为判据）
    float depth = u_GBufferDepth.SampleLevel(u_PointSampler, uv, 0.0);
    if (depth >= 1.0) discard;

    // 3. 该像素上的真实表面点 → 贴花局部空间
    float3 wp  = u_GBufferWorldPos.SampleLevel(u_PointSampler, uv, 0.0).xyz;
    float3 rel = wp - decalOrigin.xyz;
    float3 lp  = float3(dot(invRotRow0.xyz, rel),
                        dot(invRotRow1.xyz, rel),
                        dot(invRotRow2.xyz, rel));

    // 4. 投影体积裁剪：不在盒内的像素不参与（这就是"投影"与"投射片"的本质差别）
    if (any(abs(lp) > halfExtents.xyz)) discard;

    // 5. 局部 xy ∈ [-half, +half] → UV ∈ [0,1]
    float2 decalUV = lp.xy / max(halfExtents.xy * 2.0, 1e-5) + 0.5;

    // 6. 贴花纹理（无纹理 = 纯色贴花，用白色兜底）
    float4 texColor = float4(1.0, 1.0, 1.0, 1.0);
    if ((flags & 1u) != 0u) {
        uint handle = (uint)params.y;
        texColor = u_Textures[handle].Sample(u_Samplers[handle], decalUV);
    }

    // 7. 不透明度：贴花自身的 opacity × 纹理 alpha（两者都参与，便于用纹理做镂空）
    float alpha = saturate(texColor.a * colorOpacity.a);
    if (alpha <= 0.002) discard;

    // 8. 输出交给硬件按 alpha 混合（颜色 + 法线/粗糙度；金属度走 MRT0 的 alpha 通道）
    output.albedoMetallic  = float4(colorOpacity.rgb * texColor.rgb, alpha);
    output.normalRoughness = float4(normalMetal.xyz * 0.5 + 0.5, params.x);
    return output;
}
