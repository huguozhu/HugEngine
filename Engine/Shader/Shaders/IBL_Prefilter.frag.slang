// ============================================================
// IBL_Prefilter.frag.slang — 预滤波环境贴图（重要性采样 GGX）
//
// 基于 UE4 Split-Sum 方法，对 Skybox Cubemap 做重要性采样。
// 每个 mip level 对应不同的 roughness（采样方向更分散）。
// ============================================================

struct VSOutput {
    float4 position            : SV_Position;
    [[vk::location(0)]] float3 worldDir;
};

[[vk::binding(0, 0)]] TextureCube<float4> u_SkyboxTexture;
[[vk::binding(0, 0)]] SamplerState       u_SkyboxSampler;

[[vk::push_constant]] cbuffer IBLPush {
    float4x4 invViewProj;
    float    roughness;  // 当前 mip 对应的 roughness 值
    float3   _pad;
};

// 样本数（重要性采样）
static const uint SAMPLE_COUNT = 256;

// 低差异序列（Hammersley）
float2 Hammersley(uint i, uint N) {
    return float2(float(i) / float(N), float(i & 0x55555555u) * 0.001 +  // 简化
                  float(i & 0x33333333u) * 0.0001);
}

// GGX 重要性采样（UE4 方法）
float3 ImportanceSampleGGX(float2 Xi, float3 N, float roughness) {
    float a = roughness * roughness;

    float phi      = 2.0 * 3.14159265 * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    // 切线空间 → 世界空间
    float3 H;
    H.x = cos(phi) * sinTheta;
    H.y = sin(phi) * sinTheta;
    H.z = cosTheta;

    float3 up = abs(N.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 T = normalize(cross(up, N));
    float3 B = cross(N, T);

    return T * H.x + B * H.y + N * H.z;
}

float4 fragmentMain(VSOutput input) : SV_Target0 {
    float3 N = normalize(input.worldDir);
    float3 V = N;  // 对于 cubemap: viewing direction = reflection direction = normal

    float3 color = float3(0.0);
    float  totalWeight = 0.0;

    for (uint i = 0; i < SAMPLE_COUNT; i++) {
        float2 Xi = Hammersley(i, SAMPLE_COUNT);
        float3 H  = ImportanceSampleGGX(Xi, N, roughness);
        float3 L  = normalize(2.0 * dot(V, H) * H - V);  // 反射方向

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0) {
            color += u_SkyboxTexture.SampleLevel(u_SkyboxSampler, L, 0.0).rgb * NdotL;
            totalWeight += NdotL;
        }
    }

    color = totalWeight > 0.0 ? color / totalWeight : float3(0.0);
    return float4(color, 1.0);
}
