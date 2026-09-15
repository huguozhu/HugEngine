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

// === Payload（与 RT_Shadow.rahit / RT_Common.rmiss 一致）===
struct ShadowPayload {
    bool blocked;  // true=被遮挡, false=可见
};

// ============================================================
// 工具函数
// ============================================================

uint Hash(uint x) {
    x = (x ^ 61u) ^ (x >> 16u);
    x *= 9u;
    x ^= x >> 4u;
    x *= 0x27d4eb2du;
    x ^= x >> 15u;
    return x;
}

float Rand(uint2 idx, uint frame, uint s) {
    uint seed = idx.x * 1973u + idx.y * 9277u + frame * 26699u + s * 479u;
    return float(Hash(seed) & 0xFFFFu) / 65536.0;
}

// 余弦权重半球采样（绕法线 N）
float3 CosineSampleHemisphere(float3 N, float2 u) {
    float phi = 2.0 * 3.14159265 * u.x;
    float r = sqrt(u.y);
    float x = r * cos(phi);
    float y = r * sin(phi);
    float z = sqrt(max(0.0, 1.0 - u.y));
    float3 Up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T = normalize(cross(Up, N));
    float3 B = cross(N, T);
    return normalize(x * T + y * B + z * N);
}

// 从深度和 UV 重建世界坐标（Vulkan NDC y 翻转）
float3 ReconstructWorldPos(uint2 idx, float depth) {
    float2 uv = (float2(idx) + 0.5) / float2(g_PC.dispatchDimX, g_PC.dispatchDimY);
    float2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float4 clipPos = float4(ndc, depth, 1.0);
    float4 worldPos4 = mul(g_PC.invViewProj, clipPos);
    return worldPos4.xyz / worldPos4.w;
}

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

    float3 worldPos = ReconstructWorldPos(idx, depth);

    uint n = max(1u, min(g_PC.sampleCount, 8u));
    uint occluded = 0;

    for (uint s = 0; s < n; s++) {
        float2 u = float2(Rand(idx, g_PC.frameIndex, s * 2), Rand(idx, g_PC.frameIndex, s * 2 + 1));
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
