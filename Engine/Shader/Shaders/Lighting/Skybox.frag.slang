// ============================================================
// Skybox.frag.slang — 天空盒片段着色器
// ============================================================

#include "FullscreenVS.slang"   // VSOutputWorldDir / VSOutputWorldDir（接口唯一出处）

[[vk::push_constant]] cbuffer SkyboxPC {
    float4x4 invViewProjMatrix;
    float    intensity;
    float3   _pad;
};

[[vk::binding(10, 0)]] TextureCube<float4> u_SkyboxTexture;
[[vk::binding(10, 0)]] SamplerState u_SkyboxSampler;

float4 fragmentMain(VSOutputWorldDir input) : SV_Target0 {
    float3 dir = normalize(input.worldDir);
    float4 color = u_SkyboxTexture.Sample(u_SkyboxSampler, dir);
    color.rgb *= intensity;
    return color;
}
