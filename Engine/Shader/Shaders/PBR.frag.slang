// ============================================================
// PBR.frag.slang — PBR 片元着色器 (GPU-driven)
// ============================================================

#include "pbr_common.slang"

struct FSInput {
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

struct GPULight {
    float4 colorIntensity;
    float4 directionType;
    float4 positionRange;
    float2 coneAngles;
    float2 _pad;
};

// Descriptor Sets (set=0):
//   binding=1: GPULight[]
//   binding=2: GPUObjectData[]
//   binding=4: Texture2D[] + SamplerState[]
[[vk::binding(1, 0)]] StructuredBuffer<GPULight> u_Lights;
[[vk::binding(2, 0)]] StructuredBuffer<GPUObjectData> u_Objects;
[[vk::binding(4, 0)]] Texture2D u_Textures[];
[[vk::binding(4, 0)]] SamplerState u_Samplers[];

[[vk::push_constant]] cbuffer FrameConstants {
    float4x4 viewProjMatrix;
    float4   cameraPosition;
    uint     lightCount;
    uint     objectIndex;
    uint     _pad[2];
};

float3 ACESFilm(float3 x) {
    float a = 2.51; float b = 0.03; float c = 2.43; float d = 0.59; float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

float4 fragmentMain(FSInput input) : SV_Target0 {
    GPUObjectData obj = u_Objects[objectIndex];

    float3 albedo = obj.baseColorFactor.rgb;
    float alpha   = obj.baseColorFactor.a;
    if (alpha < obj.alphaCutoff) discard;

    float metallic  = obj.metallicFactor;
    float roughness = clamp(obj.roughnessFactor, 0.04, 1.0);
    float ao        = obj.aoFactor;

    float3 N = normalize(input.worldNormal);
    float3 V = normalize(cameraPosition.xyz - input.worldPos);

    // 多光源循环
    float3 color = albedo * 0.03 * ao;
    uint count = min(lightCount, 8u);
    for (uint i = 0; i < count; i++) {
        GPULight light = u_Lights[i];
        uint ltype = uint(light.directionType.w);
        float3 L; float attenuation = 1.0;
        if (ltype == 0u) {
            L = normalize(-light.directionType.xyz);
        } else {
            float3 toLight = light.positionRange.xyz - input.worldPos;
            float dist = length(toLight);
            L = toLight / max(dist, 0.001);
            float d = dist / max(light.positionRange.w, 0.001);
            attenuation = 1.0 / (1.0 + 2.0 * d + d * d);
            if (ltype == 2u) {
                float spotDot = dot(-L, normalize(light.directionType.xyz));
                attenuation *= smoothstep(light.coneAngles.y, light.coneAngles.x, spotDot);
            }
        }
        float3 radiance = light.colorIntensity.rgb * light.colorIntensity.w * attenuation;
        color += PBR_BRDF(albedo, metallic, roughness, N, V, L) * radiance;
    }
    color *= ao;
    color += obj.emissiveFactor.rgb;

    float3 mapped = ACESFilm(color);
    return float4(LinearToSRGB(mapped), alpha);
}
