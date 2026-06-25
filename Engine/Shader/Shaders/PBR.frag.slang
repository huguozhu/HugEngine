// ============================================================
// PBR.frag.slang — PBR 渲染片元着色器
//
// Cook-Torrance BRDF + 方向光 + 内联 ACES ToneMapping
// ============================================================

#include "pbr_common.slang"

// --- 输入来自顶点着色器 ---
struct FSInput {
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 worldNormal;
    [[vk::location(2)]] float2 uv;
};

// --- Push constants（与顶点着色器共享布局）---
[[vk::push_constant]] cbuffer PushConstants {
    float4x4 modelMatrix;           // [0..64]
    float4x4 viewProjMatrix;        // [64..128]
    float4   baseColorFactor;       // [128..144]
    float    metallicFactor;        // [144]
    float    roughnessFactor;       // [148]
    float    aoFactor;              // [152]
    float    alphaCutoff;           // [156]
    float4   emissiveFactor;        // [160..176]
    float4   cameraPosition;        // [176..192]
    uint     lightCount;            // [192]
    uint     materialFlags;         // [196]
    float2   lightConeAngles;       // [200]
    float4   lightColorIntensity;   // [208..224]   xyz=颜色, w=强度
    float4   lightDirectionType;    // [224..240]   xyz=方向, w=类型
    float4   lightPositionRange;    // [240..256]   xyz=位置, w=范围
};

// ============================================================
// ACES Filmic Tone Mapping
// ============================================================
float3 ACESFilm(float3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

float4 fragmentMain(FSInput input) : SV_Target0 {
    // 基础颜色
    float3 albedo = baseColorFactor.rgb;
    float alpha   = baseColorFactor.a;

    // Alpha Mask
    if (alpha < alphaCutoff) discard;

    // PBR 材质参数
    float metallic  = metallicFactor;
    float roughness = clamp(roughnessFactor, 0.04, 1.0);
    float ao        = aoFactor;

    // 几何法线 + 视线方向
    float3 N = normalize(input.worldNormal);
    float3 V = normalize(cameraPosition.xyz - input.worldPos);

    // --- 光照计算（遍历活跃光源，当前限制 1 个）---
    float3 color = albedo * 0.03 * ao;  // 环境光基础
    uint count = min(lightCount, 1u);    // 当前限制 1 个光源（B2 扩展至 Storage Buffer 多光源）

    for (uint i = 0; i < count; i++) {
        // 光线方向：Directional 用方向，Point/Spot 用位置差
        uint ltype = uint(lightDirectionType.w);
        float3 L;
        float attenuation = 1.0;
        if (ltype == 0u) {
            // Directional
            L = normalize(-lightDirectionType.xyz);
        } else {
            // Point / Spot
            float3 toLight = lightPositionRange.xyz - input.worldPos;
            float dist = length(toLight);
            L = toLight / max(dist, 0.001);
            // 距离衰减：1/(1+2*d/range+(d/range)^2)
            float d = dist / max(lightPositionRange.w, 0.001);
            attenuation = 1.0 / (1.0 + 2.0 * d + d * d);
            // Spot 锥角衰减
            if (ltype == 2u) {
                float spotDot = dot(-L, normalize(lightDirectionType.xyz));
                attenuation *= smoothstep(lightConeAngles.y, lightConeAngles.x, spotDot);
            }
        }

        float3 radiance = lightColorIntensity.rgb * lightColorIntensity.w * attenuation;
        color += PBR_BRDF(albedo, metallic, roughness, N, V, L) * radiance;
    }

    color *= ao;
    color += emissiveFactor.rgb;

    // ACES ToneMapping + sRGB 输出
    float3 mapped = ACESFilm(color);
    float3 srgb   = LinearToSRGB(mapped);
    return float4(srgb, alpha);
}
