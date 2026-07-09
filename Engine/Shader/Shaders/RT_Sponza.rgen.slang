// RT_Sponza.rgen.slang — Sponza 几何体光线生成（Phase 2）

struct RTPushConstant {
    float4x4 invViewProj;
    float4   camPosNearFar;
};
[[vk::push_constant]] RTPushConstant g_PC;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure g_TLAS;
[[vk::binding(1, 0)]] RWTexture2D<float4> g_Output;

struct RayPayload {
    float3 color;
    float  hitT;
};

[shader("raygeneration")]
void main() {
    uint2 idx = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;
    float2 uv = (float2(idx) + 0.5) / float2(dim);

    float4 ndc = float4(uv * 2.0 - 1.0, 0.5, 1.0);
    ndc.y = -ndc.y;  // GLM perspectiveRH_ZO 的 Y 映射修正（Vulkan NDC Y-down → 世界 Y-up）

    float4 worldPos4 = mul(g_PC.invViewProj, ndc);
    float3 worldPos  = worldPos4.xyz / worldPos4.w;
    float3 rayDir    = normalize(worldPos - g_PC.camPosNearFar.xyz);

    RayDesc ray;
    ray.Origin    = g_PC.camPosNearFar.xyz;
    ray.Direction = rayDir;
    ray.TMin      = 0.01;
    ray.TMax      = 10000.0;

    RayPayload payload;
    payload.color = float3(0.15, 0.22, 0.38);
    payload.hitT  = -1.0;

    TraceRay(g_TLAS, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

    g_Output[idx] = float4(payload.color, 1.0);
}
