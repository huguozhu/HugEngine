// ============================================================
// PBR.frag.slang — PBR 片元着色器
// 使用 Storage Buffer 支持多光源
// ============================================================

#include "pbr_common.slang"

struct FSInput {
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 worldNormal;
    [[vk::location(2)]] float2 uv;
};

// GPU Light（与 CPU 端 GPULight 对齐，64 bytes）
struct GPULight {
    float4 colorIntensity;      // xyz=颜色, w=强度
    float4 directionType;       // xyz=方向, w=类型(0=Dir,1=Point,2=Spot)
    float4 positionRange;       // xyz=位置, w=范围
    float2 coneAngles;          // x=内锥角, y=外锥角
    float2 _pad;
};

// Descriptor Set: set=0
//   binding=1: 光照数组
//   binding=4: Bindless 纹理数组（CombinedImageSampler[]）
[[vk::binding(1, 0)]] StructuredBuffer<GPULight> u_Lights;
[[vk::binding(4, 0)]] Texture2D u_Textures[];
[[vk::binding(4, 0)]] SamplerState u_Samplers[];

// Push constants
[[vk::push_constant]] cbuffer PushConstants {
    float4x4 modelMatrix;
    float4x4 viewProjMatrix;
    float4   baseColorFactor;
    float    metallicFactor;
    float    roughnessFactor;
    float    aoFactor;
    float    alphaCutoff;
    float4   emissiveFactor;
    float4   cameraPosition;
    uint     lightCount;
    uint     materialFlags;
    uint     materialID;            // bindless 纹理索引
    uint     _pad[13];
};

float3 ACESFilm(float3 x) {
    float a = 2.51; float b = 0.03; float c = 2.43; float d = 0.59; float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

float4 fragmentMain(FSInput input) : SV_Target0 {
    float3 albedo = baseColorFactor.rgb;
    float alpha   = baseColorFactor.a;
    if (alpha < alphaCutoff) discard;

    float metallic  = metallicFactor;
    float roughness = clamp(roughnessFactor, 0.04, 1.0);
    float ao        = aoFactor;

    float3 N = normalize(input.worldNormal);
    float3 V = normalize(cameraPosition.xyz - input.worldPos);

    // --- 多光源循环 ---
    float3 color = albedo * 0.03 * ao;
    uint count = min(lightCount, 8u);

    for (uint i = 0; i < count; i++) {
        GPULight light = u_Lights[i];
        uint ltype = uint(light.directionType.w);
        float3 L;
        float attenuation = 1.0;

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
    color += emissiveFactor.rgb;

    float3 mapped = ACESFilm(color);
    float3 srgb   = LinearToSRGB(mapped);
    return float4(srgb, alpha);
}
