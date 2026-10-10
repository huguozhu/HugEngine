#include "Sampling.slang"   // Hammersley / RadicalInverse_VdC（唯一出处）
// ============================================================
// IBL_Prefilter.frag.slang — 预滤波环境贴图（重要性采样 GGX）
//
// 基于 UE4 Split-Sum 方法，对 Skybox Cubemap 做重要性采样。
// 每个 mip level 对应不同的 roughness（采样方向更分散）。
// ============================================================

#include "FullscreenVS.slang"   // VSOutputWorldDir / VSOutputWorldDir（接口唯一出处）

[[vk::binding(0, 0)]] TextureCube<float4> u_SkyboxTexture;
[[vk::binding(0, 0)]] SamplerState       u_SkyboxSampler;

[[vk::push_constant]] cbuffer IBLPush {
    float4x4 invViewProj;
    float    roughness;  // 当前 mip 对应的 roughness 值
    float3   _pad;
};

// 样本数（重要性采样）
static const uint SAMPLE_COUNT = 256;

float4 fragmentMain(VSOutputWorldDir input) : SV_Target0 {
    float3 N = normalize(input.worldDir);
    float3 V = N;  // 对于 cubemap: viewing direction = reflection direction = normal

    float3 color = float3(0.0);
    float  totalWeight = 0.0;

    for (uint i = 0; i < SAMPLE_COUNT; i++) {
        float2 Xi = Hammersley(i, SAMPLE_COUNT);
        float3 H  = SampleGgxHalf(N, roughness, Xi, 0.0);   // 共享采样器（原本地 ImportanceSampleGGX）
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
