// ============================================================
// pbr_common.slang — PBR BRDF 函数库
//
// Cook-Torrance 微面元镜面 BRDF + Lambertian 漫反射
// ============================================================
#ifndef HUGENGINE_PBR_COMMON_SLANG
#define HUGENGINE_PBR_COMMON_SLANG

#include "common.slang"
#include "ShaderTypes.slang"  // GPU struct 共享定义（GPULight, GPUShadowData 等）

// ============================================================
// Fresnel — Schlick 近似
// ============================================================
float3 F_Schlick(float3 f0, float cosTheta) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Fresnel-Schlick with roughness: reduces mirror reflection at grazing angles
float3 F_SchlickRoughness(float cosTheta, float3 F0, float roughness, float metallic) {
    float3 rcp = max(float3(1.0 - roughness), F0);
    return F0 + (rcp - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ============================================================
// Normal Distribution Function — GGX (Trowbridge-Reitz)
// ============================================================
float D_GGX(float NdotH, float roughness) {
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denom = NdotH * NdotH * (alpha2 - 1.0) + 1.0;
    return alpha2 / (HE_PI * denom * denom);
}

// ============================================================
// 各向异性 GGX 法线分布函数（Burley 2012 / Disney）
//
// ax, ay 为沿 T/B 方向的 GGX 粗糙度 alpha；当 ax == ay 时数学上
// 精确退化为各向同性 D_GGX（HdotT² + HdotB² + NdotH² = 1）。
// ============================================================
float D_GGX_aniso(float NdotH, float HdotT, float HdotB, float ax, float ay) {
    float a2 = ax * ay;
    float3 V = float3(ay * HdotT, ax * HdotB, a2 * NdotH);
    float V2 = dot(V, V);
    float W2 = a2 / V2;
    return a2 * W2 * W2 * (1.0 / HE_PI);
}

// ============================================================
// Geometry Function — Smith + Schlick-GGX
// ============================================================
float G_SchlickGGX(float NdotV, float roughness) {
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float G_Smith(float NdotV, float NdotL, float roughness) {
    return G_SchlickGGX(NdotV, roughness) * G_SchlickGGX(NdotL, roughness);
}

// ============================================================
// PCF 阴影采样（Percentage-Closer Filtering）
//
// 3x3 核比较采样，返回 [0,1] 可见度。
// 1.0 = 完全照亮，0.0 = 完全遮蔽。
// ============================================================
float SampleShadowPCF(float3 worldPos, float3 worldNormal, float3 lightDir,
                       GPUShadowData sm, float3 cameraPos,
                       Texture2D<float> shadowMap0,
                       Texture2D<float> shadowMap1,
                       Texture2D<float> shadowMap2,
                       SamplerState shadowSampler)
{
    // CSM 级联选择
    float viewZ = dot(worldPos - cameraPos, sm.cameraForward.xyz);
    uint cascade = 0u;
    if (viewZ > sm.splitDistances[0]) cascade = 1u;
    if (viewZ > sm.splitDistances[1]) cascade = 2u;

    float4x4 lightViewProj = sm.lightViewProj[cascade];
    float3 biasedPos = worldPos + worldNormal * sm.shadowParams.y;

    float4 shadowClip = mul(lightViewProj, float4(biasedPos, 1.0));
    float3 ndc = shadowClip.xyz / shadowClip.w;
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    float  depth = ndc.z - sm.shadowParams.x;

    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return 1.0;

    float2 texelSize = 1.0 / 2048.0;
    float shadow = 0.0;
    if (cascade == 0u) {
        for (int x = -1; x <= 1; x++)
            for (int y = -1; y <= 1; y++)
                shadow += (depth <= shadowMap0.Sample(shadowSampler, uv + float2(x, y) * texelSize)) ? 1.0 : 0.0;
    } else if (cascade == 1u) {
        for (int x = -1; x <= 1; x++)
            for (int y = -1; y <= 1; y++)
                shadow += (depth <= shadowMap1.Sample(shadowSampler, uv + float2(x, y) * texelSize)) ? 1.0 : 0.0;
    } else {
        for (int x = -1; x <= 1; x++)
            for (int y = -1; y <= 1; y++)
                shadow += (depth <= shadowMap2.Sample(shadowSampler, uv + float2(x, y) * texelSize)) ? 1.0 : 0.0;
    }
    return shadow / 9.0;
}

// ============================================================
// 点光源阴影 — Cubemap 单次采样
//
// 从片段位置向光源方向采样 Cubemap，比较距离。
// 返回 [0,1] 可见度（1=完全照亮，0=完全遮蔽）。
// ============================================================
float SamplePointShadow(float3 worldPos, float3 lightPos, float lightRange,
                         GPUShadowData sm, TextureCube<float> shadowCube,
                         SamplerState shadowSampler)
{
    float3 toLight = lightPos - worldPos;
    float  dist    = length(toLight);
    float3 dir     = toLight / max(dist, 0.001);

    // 超出范围 → 无阴影
    if (dist > lightRange)
        return 1.0;

    // 深度采样（方向向量直接用作 Cubemap 纹理坐标）
    float shadowDepth = shadowCube.Sample(shadowSampler, dir).r;

    // 归一化距离用于比较：世界空间距离 / 光源范围
    float normalizedDist = dist / lightRange;

    // 贴图深度是透视投影深度（n=0.1, f=lightRange，非线性）。
    // 反算线性距离后再归一化比较，否则线性距离 vs 非线性深度恒失配（阴影失效）。
    const float kPointShadowNear = 0.1;
    float linDepth = kPointShadowNear * lightRange
                   / (lightRange - shadowDepth * (lightRange - kPointShadowNear));

    // 应用深度偏移后比较（深度偏移在归一化空间）
    return (normalizedDist - sm.shadowParams.x <= linDepth / lightRange) ? 1.0 : 0.0;
}

// ============================================================
// PBR BRDF — Cook-Torrance + Lambertian
// ============================================================
float3 PBR_BRDF(float3 albedo, float metallic, float roughness,
                float3 N, float3 V, float3 L,
                float dielectricF0 = 0.04,  // 默认 0.04 保持 ReSTIR/PT 调用点不变
                float2 envBRDF = float2(0.0, 1.0),  // envBRDF 默认 (0,1) 表示无能量补偿
                float4 disneyA = float4(0, 0, 0.5, 0),   // anisotropic, subsurface, specular, sheen
                float4 disneyB = float4(0, 1, 1, 1),      // clearcoat, clearcoatGloss, specularTint.rg
                float  disneyC = 1.0)                     // specularTint.b
{
    float anisotropic = disneyA.x, subsurface = disneyA.y, specular = disneyA.z, sheen = disneyA.w;
    float clearcoat = disneyB.x, clearcoatGloss = disneyB.y;
    float3 specularTint = float3(disneyB.z, disneyB.w, disneyC);

    float3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    // 各向异性切线基：由法线构造标准正交系（anisotropic=0 时不影响结果）
    float3 T, B;
    if (abs(N.y) < 0.999)
        T = normalize(cross(float3(0, 1, 0), N));
    else
        T = normalize(cross(float3(1, 0, 0), N));
    B = normalize(cross(N, T));
    float HdotT = dot(H, T);
    float HdotB = dot(H, B);

    // specular/specularTint：电介质 F0 = 0.16*specular²（specular!=0.5 时覆盖 ior 派生 F0）
    float dielectric = (abs(specular - 0.5f) > 1e-4f) ? (0.16f * specular * specular) : dielectricF0;
    // 菲涅尔反射率：电介质 = dielectricF0（由 IOR 预计算），金属 = albedo
    float3 f0 = lerp(float3(dielectric), albedo, metallic) * specularTint;

    // Cook-Torrance 镜面项
    float3 F = F_Schlick(f0, HdotV);
    // 各向异性 GGX：aspect = sqrt(1-0.9*anisotropic)，ax=alpha/aspect，ay=alpha*aspect
    float aspect = sqrt(1.0 - 0.9 * anisotropic);
    float alpha = roughness * roughness;
    float D = (anisotropic > 0.0)
            ? D_GGX_aniso(NdotH, HdotT, HdotB, alpha / aspect, alpha * aspect)
            : D_GGX(NdotH, roughness);
    float  G = G_Smith(NdotV, NdotL, roughness);

    float3 specularBRDF = (D * G * F) / max(4.0 * NdotV * NdotL, HE_EPSILON);

    // ── 多重散射能量补偿（Kulla-Conty / Frostbite）──
    // 方向反照率 E(f0) = f0 * A + B（split-sum LUT 的 scale/bias 即 Schlick Fresnel 分解的两项）
    // envBRDF 由调用方采样 u_BRDF_LUT(NdotV, roughness) 传入；默认 (0,1) 表示无补偿
    float3 directionalAlbedo = f0 * envBRDF.x + envBRDF.y;
    float3 energyCompensation = 1.0 + f0 * (1.0 / max(directionalAlbedo, 0.001) - 1.0);
    specularBRDF *= energyCompensation;

    // Lambertian 漫反射项（金属无漫反射）
    float3 kD = (1.0 - F) * (1.0 - metallic);
    float3 diffuse = kD * albedo / HE_PI;

    // subsurface：diffuse = mix(Lambert, wrapDiffuse, subsurface)，wrap 余弦 (NdotL+ss)/(1+ss)
    float NdotL_wrap = (NdotL + subsurface) / (1.0 + subsurface);
    if (NdotL > 0.0) diffuse *= lerp(NdotL, NdotL_wrap, subsurface) / NdotL;

    // sheen：边缘项 F_sheen = sheen * pow(1-HdotV,5) * albedo 加到漫反射
    diffuse += albedo * (sheen * pow(1.0 - HdotV, 5.0));

    // 基础 BRDF = 漫反射 + 镜面（末尾统一乘 NdotL）
    float3 baseBRDF = diffuse + specularBRDF;

    // clearcoat：第二层镜面（IOR 1.5，GGX），按 clearcoat 强度混合
    float Fcc = 0.04 + 0.96 * pow(1.0 - HdotV, 5.0);   // 清漆层菲涅尔（IOR 1.5 → F0=0.04）
    float Dcc = D_GGX(NdotH, clearcoatGloss);           // 内部 alpha = clearcoatGloss²
    float Gcc = G_Smith(NdotV, NdotL, clearcoatGloss);
    float3 clearcoatLobe = (Dcc * Gcc) / max(4.0 * NdotV * NdotL, HE_EPSILON);   // 不含 Fcc（Fcc 在下方混合时应用一次）
    baseBRDF = lerp(baseBRDF, (1.0 - Fcc) * baseBRDF + Fcc * clearcoatLobe, clearcoat);

    return baseBRDF * NdotL;
}

#endif // HUGENGINE_PBR_COMMON_SLANG
