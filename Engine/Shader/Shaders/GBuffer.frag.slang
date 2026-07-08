// GBuffer.frag — 延迟渲染 GBuffer 片段着色器（4 MRT + velocity）
// MRT 0: RGBA16  albedo.rgb + metallic
// MRT 1: RGBA16  worldNormal.xyz * 0.5 + 0.5 + roughness
// MRT 2: RGBA16  emissive.rgb + ao
// MRT 3: RG16    velocity.xy = currUV - prevUV（UV 空间运动矢量）

#include "common.slang"

struct VSOutput {
    float4 position              : SV_Position;
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 worldNormal;
    [[vk::location(2)]] float2 uv;
    [[vk::location(3)]] float4 prevClipPos;
};

// GPUObjectData 定义来自 ShaderTypes.slang（C++/Slang 共享）

// set=0: per-frame data (u_Objects + bindless textures)
[[vk::binding(2, 0)]] StructuredBuffer<GPUObjectData> u_Objects;

// Bindless 纹理通过 common.slang 中的 u_Textures[] / u_Samplers[] 访问
// 偏移: materialID + 0 = BaseColor, +1 = Normal, +2 = MetallicRoughness, +3 = Occlusion

[[vk::push_constant]] cbuffer FrameConstants {
    float4x4 viewProjMatrix;
    float4x4 prevViewProjMatrix;
    uint     objectIndex;
    uint     _pad[15];
};

struct FSOutput {
    [[vk::location(0)]] float4 albedoMetallic  : SV_Target0;
    [[vk::location(1)]] float4 normalRoughness : SV_Target1;
    [[vk::location(2)]] float4 emissiveAO      : SV_Target2;
    [[vk::location(3)]] float2 velocity         : SV_Target3;  // UV 空间运动矢量
};

FSOutput fragmentMain(VSOutput input) {
    GPUObjectData obj = u_Objects[objectIndex];

    // ── MRT 0-2：PBR 材质（bindless 纹理数组采样） ──
    uint texBase = obj.materialID;
    float4 baseColorSample = u_Textures[texBase + 0].Sample(u_Samplers[0], input.uv);
    float3 albedo   = obj.baseColorFactor.rgb * baseColorSample.rgb;
    float  alpha    = obj.baseColorFactor.a * baseColorSample.a;
    float  metallic = obj.metallicFactor * u_Textures[texBase + 2].Sample(u_Samplers[0], input.uv).b;

    if (alpha < obj.alphaCutoff) discard;

    float3 texNormal = u_Textures[texBase + 1].Sample(u_Samplers[0], input.uv).rgb * 2.0 - 1.0;
    float3 N = normalize(input.worldNormal + texNormal * 0.5);

    float roughness = clamp(obj.roughnessFactor *
        u_Textures[texBase + 2].Sample(u_Samplers[0], input.uv).g, 0.04, 1.0);
    float ao = obj.aoFactor * u_Textures[texBase + 3].Sample(u_Samplers[0], input.uv).r;

    // ── MRT 3：Velocity（UV 空间运动矢量 = currUV - prevUV） ──
    float3 currNDC = input.position.xyz / input.position.w;
    float2 currUV  = currNDC.xy * 0.5 + 0.5;

    float3 prevNDC = input.prevClipPos.xyz / input.prevClipPos.w;
    float2 prevUV  = prevNDC.xy * 0.5 + 0.5;

    float2 motion = currUV - prevUV;

    FSOutput output;
    output.albedoMetallic  = float4(albedo, metallic);
    output.normalRoughness = float4(N * 0.5 + 0.5, roughness);
    output.emissiveAO      = float4(obj.emissiveFactor.rgb, ao);
    output.velocity        = motion;
    return output;
}
