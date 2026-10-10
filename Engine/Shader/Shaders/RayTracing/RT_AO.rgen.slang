// RT_AO.rgen.slang — 硬件 Ray Tracing 环境光遮蔽射线生成着色器
//
// 对 GBuffer 有效像素在法线半球余弦采样 N 条射线，
// 命中（AnyHit 置 blocked）即视为遮蔽；输出 1-遮蔽率 到 R8_UNORM。
//
// 复用 RT_Shadow.rahit（AnyHit 置 blocked）+ RT_Common.rmiss（未命中置 false）
// 光线 TMax = maxDistance（2m）→ 仅附近几何体产生遮蔽。
//
// 输出由 LightingPass 在 rtAOSource=1 时采样（u_RT_AO）。

#include "ShaderTypes.slang"  // RTRayEffectPushConstant

// === RayGen 资源 (set=0) ===
[[vk::binding(0, 0)]] RaytracingAccelerationStructure g_TLAS;
[[vk::binding(1, 0)]] RWTexture2D<float> g_Output;      // R8_UNORM 遮蔽值（0=全遮蔽, 1=无遮蔽）
[[vk::binding(2, 0)]] Texture2D<float>  g_Depth;         // GBuffer 深度
[[vk::binding(3, 0)]] Texture2D<float4> g_NormalRough;   // GBufferB: worldNormal.xyz

// === Push Constant ===
[[vk::push_constant]] RTRayEffectPushConstant g_PC;

#include "RT_Common.slang"   // Hash / RandHash / ReconstructWorldPosFromIndex（唯一出处）
// === Payload（与 RT_Shadow.rahit / RT_Common.rmiss 一致）===
#include "RTPayloads.slang"   // ShadowPayload / RayPayload / GIPayload / ReflectionPayload（唯一出处）

// ============================================================
// 工具函数
// ============================================================

// ============================================================
// 主入口：RayGen 着色器
// ============================================================
[shader("raygeneration")]
void main() {
    uint2 idx = DispatchRaysIndex().xy;

    float depth = g_Depth.Load(int3(idx, 0)).r;
    if (depth >= 1.0 || depth <= 0.0) {
        g_Output[idx] = 1.0;  // 天空像素 → 无遮蔽
        return;
    }
    float4 gbB = g_NormalRough.Load(int3(idx, 0));
    float3 N = normalize(gbB.xyz * 2.0 - 1.0);

    float3 worldPos = ReconstructWorldPosFromIndex(idx, depth);

    uint n = max(1u, min(g_PC.sampleCount, 8u));
    uint occluded = 0;

    for (uint s = 0; s < n; s++) {
        float2 u = float2(RandHash(idx, g_PC.frameIndex, s * 2), RandHash(idx, g_PC.frameIndex, s * 2 + 1));
        float3 dir = CosineSampleHemisphere(N, u);

        RayDesc ray;
        ray.Origin    = worldPos + N * 0.02;   // 沿法线偏移避免自交
        ray.Direction = dir;
        ray.TMin      = 0.02;
        ray.TMax      = g_PC.maxDistance;      // 仅统计遮蔽半径内命中

        ShadowPayload payload;
        payload.blocked = false;
        TraceRay(g_TLAS,
            RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
            0xFF, 0, 0, 0, ray, payload);

        if (payload.blocked) occluded++;
    }

    float ao = 1.0 - float(occluded) / float(n);
    g_Output[idx] = ao;
}
