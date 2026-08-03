// RT_GI.rgen.slang — 硬件 Ray Tracing 间接漫反射（GI）射线生成着色器
//
// 对 GBuffer 有效像素在法线半球余弦采样 N 条射线（SPP=1-2），
// ClosestHit 评估命中表面出射辐射度（一次反弹），Miss 回退 DDGI 探针查询（替代纯天空色）。
// 输出 RGBA16_FLOAT 四分之一分辨率间接漫反射（rgb + 有效性 a）。
//
// 与 DDGI 配合：RT GI 覆盖中距离（5-30m）精确间接光，
// DDGI 覆盖远距离低频 GI（LightingPass 中两者叠加）。
//
// 输出由 LightingPass 在 rtDiffuseSource=1 时采样（u_RT_GI）。

#include "ShaderTypes.slang"  // RTRayEffectPushConstant
#include "RT_DDGI.slang"     // DDGI 探针查询共享库（miss 回退低频间接光）

// === RayGen 资源 (set=0) ===
[[vk::binding(0, 0)]] RaytracingAccelerationStructure g_TLAS;
[[vk::binding(1, 0)]] RWTexture2D<float4> g_Output;   // RGBA16F 间接漫反射
[[vk::binding(2, 0)]] Texture2D<float>  g_Depth;       // GBuffer 深度
[[vk::binding(3, 0)]] Texture2D<float4> g_NormalRough; // GBufferB: worldNormal.xyz
[[vk::binding(7, 0)]] StructuredBuffer<float4> u_DDGIProbes;  // DDGI 探针（miss 回退）

// === Push Constant ===
[[vk::push_constant]] RTRayEffectPushConstant g_PC;

// === Payload（与 ClosestHit/Miss 共享）===
struct GIPayload {
    float4 radianceT;  // rgb=辐射度, a=命中距离（-1=未命中）
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
        g_Output[idx] = float4(0, 0, 0, 0);  // 天空像素无间接光
        return;
    }
    float4 gbB = g_NormalRough.Load(int3(idx, 0));
    float3 N = normalize(gbB.xyz * 2.0 - 1.0);

    float3 worldPos = ReconstructWorldPos(idx, depth);

    uint n = max(1u, min(g_PC.sampleCount, 8u));
    float3 radiance = float3(0, 0, 0);
    uint valid = 0;

    for (uint s = 0; s < n; s++) {
        float2 u = float2(Rand(idx, g_PC.frameIndex, s * 2), Rand(idx, g_PC.frameIndex, s * 2 + 1));
        float3 dir = CosineSampleHemisphere(N, u);

        RayDesc ray;
        ray.Origin    = worldPos + N * 0.02;
        ray.Direction = dir;
        ray.TMin      = 0.02;
        ray.TMax      = g_PC.maxDistance;   // 30m 追踪范围

        GIPayload payload;
        payload.radianceT = float4(0, 0, 0, -1);
        TraceRay(g_TLAS, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

        if (payload.radianceT.a < 0.0) {
            // miss：DDGI 探针查询替代纯天空色（远距离低频间接光）
            radiance += SampleDDGI(worldPos, dir);
        } else {
            // hit：ClosestHit 返回的出射辐射度
            radiance += payload.radianceT.rgb;
        }
        valid++;   // 每条射线（命中或 DDGI 回退）都提供了间接光，计为有效
    }
    radiance /= float(n);

    // 时域抖动：单采样时输出原始值，由后续帧累积（或交由 Lighting 叠加）
    g_Output[idx] = float4(radiance, valid > 0 ? 1.0 : 0.0);
}
