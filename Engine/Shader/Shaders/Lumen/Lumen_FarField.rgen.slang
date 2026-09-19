// Lumen_FarField.rgen.slang — Lumen 远场硬件光追（步骤 26）
//
// 【做什么】对步骤 21 的**每一批探针光线**再发一条硬件光追光线（同一位置、同一方向 ——
// 方向由 `Lumen/ScreenProbeSampling.slang` 的同一套确定性随机给出，两个 pass 必然一致），
// 把命中距离与命中辐射度写进缓冲。近场仍由 SDF sphere tracing 负责（步骤 27 才做切换与淡入），
// 本步先把"远场到底命中了什么"量出来：**远场命中与三角形场景是否一致**。
//
// 【为什么复用 RTGI 的 rchit/rmiss】远场命中的辐射度必须与 RTGI 同源、同量纲，否则步骤 27 的
// 切换会在阈值处出现阶跃。命中着色（材质 + 直接光）整套逻辑都在 `RT_GI.rchit` 里，直接复用。
// set0 的 b0/b1/b4/b5/b6/b7 布局与 `DDGITracePass` 对齐（b0=TLAS, b1=输出 SSBO,
// b4=场景材质纹理, b5=命中点光源 UBO, b6=三角形法线, b7=本 pass 参数）。
#include "ShaderTypes.slang"
#include "Lumen/LumenFarField.slang"
#include "Lumen/ScreenProbe.slang"
#include "Lumen/ScreenProbeSampling.slang"

[[vk::binding(0, 0)]] RaytracingAccelerationStructure g_TLAS;
// **每条光线两个 float4**：[2i] = (命中距离, 是否命中, 命中辐射度亮度, 未用)
//                       [2i+1] = (命中点世界坐标, 1)  —— 合并 pass 需要命中点才能改写命中位置
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> g_FarField;
[[vk::binding(2, 0)]] StructuredBuffer<ScreenProbe> g_Probes;
[[vk::binding(7, 0)]] cbuffer LumenFarFieldCB {
    LumenFarFieldParams g_Params;
};

[[vk::push_constant]] RTRayEffectPushConstant g_PC;

[shader("raygeneration")]
void main() {
    const uint flat = DispatchRaysIndex().x;
    const uint N    = max(1u, g_Params.dims.y);
    const uint probeIndex = flat / N;
    const uint rayIndex   = flat % N;
    if (probeIndex >= g_Params.dims.x) return;

    const ScreenProbe probe = g_Probes[probeIndex];
    if (probe.position.w < 0.5) {
        g_FarField[flat * 2u]     = float4(-1.0, 0.0, 0.0, 0.0);
        g_FarField[flat * 2u + 1u] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    const float3 ro = probe.position.xyz;
    const float3 n  = normalize(probe.normal.xyz);
    const float  roughness = clamp(probe.normal.w, 0.05, 1.0);

    // 与追踪 pass 完全相同的"探针身份 → 随机流"编号（不能跟槽位走，见 ScreenProbe_Trace 的说明）
    const uint  probeKey = probe.meta.y * 4u + probe.meta.w;
    const float2 u = ProbeRayRandom(probeKey * N + rayIndex, g_Params.dims.z);
    const float3 rd = (g_Params.dims.w == 0u) ? SampleGgxDirection(n, roughness, u)
                                             : SampleUniformHemisphere(n, u);

    RayDesc ray;
    // 起点抬高 1 个 eps：与 SDF 追踪同一起点约定，避免自交
    ray.Origin    = ro + n * 0.02;
    ray.Direction = rd;
    ray.TMin      = max(0.0, g_Params.range.x);
    ray.TMax      = max(g_Params.range.x + 0.01, g_Params.range.y);

    LumenFarFieldPayload payload;
    payload.radianceT = float4(0.0, 0.0, 0.0, -1.0);
    TraceRay(g_TLAS, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

    if (payload.radianceT.a < 0.0) {   // 未命中（miss 着色器不写 a ⇒ 保持 -1）
        g_FarField[flat * 2u]     = float4(-1.0, 0.0, 0.0, 0.0);
        g_FarField[flat * 2u + 1u] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    const float  t   = payload.radianceT.a;
    const float3 rad = payload.radianceT.rgb;
    const float  lum = dot(rad, float3(0.2126, 0.7152, 0.0722));
    g_FarField[flat * 2u]      = float4(t, 1.0, lum, 0.0);
    g_FarField[flat * 2u + 1u] = float4(ray.Origin + rd * t, 1.0);   // 命中点（合并 pass 用）
}
