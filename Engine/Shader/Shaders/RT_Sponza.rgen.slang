// RT_Sponza.rgen.slang — 多采样光线生成（可调 SPP）

struct RTPushConstant {
    float4x4 invViewProj;
    float4   camPosNearFar;  // xyz=camera pos, w=nearPlane
    uint     sampleCount;     // 每像素采样数（1-16）
    uint     frameIndex;      // 帧序号（抖动种子）
    uint     _pad0;
    uint     _pad1;
};
[[vk::push_constant]] RTPushConstant g_PC;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure g_TLAS;
[[vk::binding(1, 0)]] RWTexture2D<float4> g_Output;

struct RayPayload {
    float3 color;
    float  hitT;
};

// 分层抖动：将像素划分为 √N × √N 网格，每个采样取一个子格中心
float2 StratifiedJitter(uint sampleIdx, uint sampleCount) {
    uint gridSize = uint(ceil(sqrt(float(sampleCount))));
    uint sx = sampleIdx % gridSize;
    uint sy = sampleIdx / gridSize;
    // 在子格内随机偏移（用 sampleIdx+frameIndex 做种子避免帧间重复）
    uint seed = sampleIdx + g_PC.frameIndex * sampleCount;
    float rx = float((seed * 1664525u + 1013904223u) & 0xFFFF) / 65536.0;
    float ry = float((seed * 22695477u + 1u) & 0xFFFF) / 65536.0;
    return float2((float(sx) + rx) / float(gridSize),
                  (float(sy) + ry) / float(gridSize));
}

[shader("raygeneration")]
void main() {
    uint2 idx = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;

    uint n = max(1u, min(g_PC.sampleCount, 64u));  // 限制 1-64 SPP

    float3 color = float3(0, 0, 0);
    for (uint s = 0; s < n; s++) {
        float2 jitter = StratifiedJitter(s, n);
        float2 uv = (float2(idx) + jitter) / float2(dim);

        float4 ndc = float4(uv * 2.0 - 1.0, 0.5, 1.0);
        ndc.y = -ndc.y;

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

        color += payload.color;
    }
    color /= float(n);

    g_Output[idx] = float4(color, 1.0);
}
