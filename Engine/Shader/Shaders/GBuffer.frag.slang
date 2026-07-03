// GBuffer.frag — Deferred GBuffer 片段着色器（3 MRT）
// MRT 0: RGBA16  albedo.rgb + metallic
// MRT 1: RGBA16  worldNormal.xyz * 0.5 + 0.5 + roughness
// MRT 2: RGBA16  emissive.rgb + ao

struct VSOutput {
    float4 position              : SV_Position;
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 worldNormal;
    [[vk::location(2)]] float2 uv;
};

struct GPUObjectData {
    float4x4 worldMatrix;
    float4   baseColorFactor;
    float4   emissiveFactor;
    float    metallicFactor;
    float    roughnessFactor;
    float    aoFactor;
    float    alphaCutoff;
    uint     materialFlags;
    uint     materialID;
    uint     _pad[2];
};

// set=0: per-frame GPUObjectData[]
[[vk::binding(2, 0)]] StructuredBuffer<GPUObjectData> u_Objects;
// set=1: per-mesh 静态纹理（永不更新，消除帧间竞态）
[[vk::binding(5, 1)]] Texture2D<float4> u_BaseColorTex;
[[vk::binding(5, 1)]] SamplerState       u_BaseColorSampler;
[[vk::binding(6, 1)]] Texture2D<float4> u_NormalTex;
[[vk::binding(6, 1)]] SamplerState       u_NormalSampler;
[[vk::binding(7, 1)]] Texture2D<float4> u_MetallicRoughnessTex;
[[vk::binding(7, 1)]] SamplerState       u_MetallicRoughnessSampler;
[[vk::binding(8, 1)]] Texture2D<float4> u_OcclusionTex;
[[vk::binding(8, 1)]] SamplerState       u_OcclusionSampler;

[[vk::push_constant]] cbuffer FrameConstants {
    float4x4 viewProjMatrix;
    uint     objectIndex;
    uint     _pad[12];
};

struct FSOutput {
    [[vk::location(0)]] float4 albedoMetallic  : SV_Target0;
    [[vk::location(1)]] float4 normalRoughness : SV_Target1;
    [[vk::location(2)]] float4 emissiveAO      : SV_Target2;
};

FSOutput fragmentMain(VSOutput input) {
    GPUObjectData obj = u_Objects[objectIndex];

    float4 baseColorSample = u_BaseColorTex.Sample(u_BaseColorSampler, input.uv);
    float3 albedo   = obj.baseColorFactor.rgb * baseColorSample.rgb;
    float  alpha    = obj.baseColorFactor.a * baseColorSample.a;
    float  metallic = obj.metallicFactor * u_MetallicRoughnessTex.Sample(u_MetallicRoughnessSampler, input.uv).b;

    if (alpha < obj.alphaCutoff) discard;

    // 法线贴图
    float3 texNormal = u_NormalTex.Sample(u_NormalSampler, input.uv).rgb * 2.0 - 1.0;
    float3 N = normalize(input.worldNormal + texNormal * 0.5);

    float roughness = clamp(obj.roughnessFactor *
        u_MetallicRoughnessTex.Sample(u_MetallicRoughnessSampler, input.uv).g, 0.04, 1.0);
    float ao = obj.aoFactor * u_OcclusionTex.Sample(u_OcclusionSampler, input.uv).r;

    FSOutput output;
    output.albedoMetallic  = float4(albedo, metallic);
    output.normalRoughness = float4(N * 0.5 + 0.5, roughness);
    output.emissiveAO      = float4(obj.emissiveFactor.rgb, ao);
    return output;
}
