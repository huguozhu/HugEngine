// ============================================================
// PT_Common.slang — 全路径追踪共享函数库
//
// 提供：
//   1. PathPayload（RayGen/ClosestHit/Miss 共享的 48B 载荷）
//   2. 随机数工具（STBN 时空蓝噪声采样，确定性随机流）
//   3. BSDF 采样（GGX 镜面 + 余弦漫反射混合，返回混合 pdf）
//   4. MIS 幂次启发、光源采样（GPULight）、天空采样
//   5. 深度重建世界坐标 / 世界坐标 → UV 投影（velocity 用）
//
// 被 PT_Full.rgen / PT_Full.rchit / PT_Full.rmiss 与
// ReSTIR_Init / ReSTIR_Temporal / ReSTIR_Spatial 共享。
// ============================================================
#ifndef HUGENGINE_PT_COMMON_SLANG
#define HUGENGINE_PT_COMMON_SLANG

#include "ShaderTypes.slang"   // PTPushConstant / PTReservoir / GPULight
#include "pbr_common.slang"    // D_GGX / F_Schlick / PBR_BRDF（含 common.slang）

// ============================================================
// PathPayload — 路径追踪载荷（rchit/rmiss 定义必须逐字节一致）
// 48B：albedoMetallic + normalRough + emissiveT
// ============================================================
struct PathPayload {
    float4 albedoMetallic;  // rgb=albedo, a=metallic
    float4 normalRough;     // xyz=normal(世界空间), w=roughness
    float4 emissiveT;       // rgb=emissive, a=hitT（miss: a=-1, rgb=sky）
};

// ============================================================
// 随机数工具（确定性，像素索引 + 帧号 + 样本号）
// 随机流由 STBN 时空蓝噪声 3D 纹理提供（g_BlueNoise 由各使用方 shader 声明），
// 相同 idx/frame/s → 同值（保持确定性），且空间/时间上更均匀。
// ============================================================

// STBN 采样：空间 (x,y) 取模 128，时间切层 (frame + offset) % 64。
// 用 Load() 整数取像素（Texture3D 需 int4：xyz + mip=0）——无采样器，
// 规避 RT 管线组合采样器坑。
float SampleBlueNoise1(uint2 idx, uint frame, uint offset) {
    uint z = (frame + offset * 23u) % 64u;
    return g_BlueNoise.Load(int4(idx.x & 127u, idx.y & 127u, z, 0)).r;
}

float Rand(uint2 idx, uint frame, uint s) {
    return SampleBlueNoise1(idx, frame, s);
}

uint RandInt(uint2 idx, uint frame, uint s, uint n) {
    // 用 g 通道 + 额外空间偏移，避免与 Rand 同层同坐标（降低相关性）
    uint z = (frame + s * 29u) % 64u;
    float v = g_BlueNoise.Load(int4((idx.x + 37u) & 127u, (idx.y + 61u) & 127u, z, 0)).g;
    return uint(v * float(max(n, 1u))) % max(n, 1u);
}

// 分层抖动：将像素划分为 √N × √N 网格，每个采样取一个子格中心（STBN 偏移）
float2 StratifiedJitter(uint sampleIdx, uint sampleCount, uint frameIndex) {
    uint gridSize = uint(ceil(sqrt(float(sampleCount))));
    uint sx = sampleIdx % gridSize;
    uint sy = sampleIdx / gridSize;
    float2 bn = g_BlueNoise.Load(int4(sx & 127u, sy & 127u, (frameIndex + sampleIdx * 11u) % 64u, 0)).xy;
    return float2((float(sx) + bn.x) / float(gridSize),
                  (float(sy) + bn.y) / float(gridSize));
}

// ============================================================
// 方向采样
// ============================================================

// 余弦权重半球采样（绕法线 N），pdf = NdotL / PI
float3 CosineSampleHemisphere(float3 N, float2 u) {
    float phi = 2.0 * 3.14159265 * u.x;
    float r = sqrt(u.y);
    float x = r * cos(phi);
    float y = r * sin(phi);
    float z = sqrt(max(0.0, 1.0 - u.y));
    float3 Up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T = normalize(cross(Up, N));
    float3 B = cross(N, T);
    return normalize(x * T + y * B + z * N);
}

// GGX 半向量重要性采样（绕法线 N），pdf_H = D_GGX(NdotH) * NdotH
float3 SampleGGXHalf(float3 N, float roughness, float2 u, out float NdotH) {
    float alpha = max(roughness * roughness, 0.001);
    float cosTheta = sqrt((1.0 - u.y) / (1.0 + (alpha * alpha - 1.0) * u.y));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float phi = 6.2831853 * u.x;
    float3 H = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
    float3 Up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T = normalize(cross(Up, N));
    float3 B = cross(N, T);
    NdotH = cosTheta;
    return normalize(H.x * T + H.y * B + H.z * N);
}

// ============================================================
// 混合 BSDF 采样（漫反射余弦 + GGX 镜面，单样本模型）
// 返回采样方向 wi 与完整混合 pdf（两瓣 pdf 之和，保证无偏）
// ============================================================
float3 SampleBSDF(float3 N, float3 V, float metallic, float roughness,
                  float2 uLobe, float2 uDir, out float pdf) {
    // 镜面采样概率：金属 >90% 镜面，介质保留 35% 镜面（F0=0.04 不可忽略）
    float pSpec = lerp(0.35, 0.95, metallic);
    float pDiff = 1.0 - pSpec;

    float3 wi;
    if (uLobe.x < pDiff) {
        // 漫反射：余弦半球
        wi = CosineSampleHemisphere(N, uDir);
        float pdfCos = max(dot(N, wi), 0.0) / 3.14159265;
        // 镜面瓣在同一方向的 pdf（半向量法）
        float NdotV = max(dot(N, V), 1e-4);
        float3 H = normalize(V + wi);
        float NdotH = max(dot(N, H), 0.0);
        float pdfGgx = D_GGX(NdotH, roughness) * NdotH / (4.0 * NdotV);
        pdf = pDiff * pdfCos + pSpec * pdfGgx;
    } else {
        // 镜面：GGX 半向量采样 → 反射方向
        float NdotH;
        float3 H = SampleGGXHalf(N, roughness, uDir, NdotH);
        wi = normalize(reflect(-V, H));
        if (dot(N, wi) <= 0.0) { pdf = 0.0; return float3(0, 0, 1); }
        float NdotV = max(dot(N, V), 1e-4);
        float pdfGgx = D_GGX(NdotH, roughness) * NdotH / (4.0 * NdotV);
        float pdfCos = max(dot(N, wi), 0.0) / 3.14159265;
        pdf = pDiff * pdfCos + pSpec * pdfGgx;
    }
    return wi;
}

// ============================================================
// MIS — 幂次启发（power heuristic）
// ============================================================
float PowerHeuristic(float a, float b) {
    return (a * a) / max(a * a + b * b, 1e-6);
}

// ============================================================
// 天空采样 — 渐变天空（与 RT_Sponza.rmiss 同款色板）
// ============================================================
float3 SampleSky(float3 direction, float skyIntensity) {
    float t = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    float3 skyBottom = float3(0.55, 0.45, 0.35);   // 暖色地面
    float3 skyTop    = float3(0.25, 0.45, 0.7);    // 蓝色天顶
    return lerp(skyBottom, skyTop, t) * skyIntensity;
}

// ============================================================
// 光源采样 — 从 GPULight 计算采样位置/方向/辐射度/概率密度
// 方向光/点光源均为确定性采样（pdf=1，单点光源为 delta 分布）
// ============================================================
bool SampleLight(GPULight light, float3 P,
                 out float3 lightPos, out float3 L, out float3 Li, out float lightPdf) {
    lightPdf = 1.0;
    if (light.colorIntensity.w <= 0.0) return false;

    if (light.directionType.w == 0.0) {
        // 方向光：光强与距离无关，光源位置取任意远处点。
        // L 指向光源，须取反——direction 字段为光传播方向（与光栅化 Deferred/PBR 管线一致）。
        L = normalize(-light.directionType.xyz);
        lightPos = P + L * 10000.0;
        Li = light.colorIntensity.rgb * light.colorIntensity.w;
        return true;
    }

    // 点/聚光灯：方向指向光源，距离衰减 saturate(1 - d/range)^2（与 RT_HitCommon 一致）
    float range = abs(light.positionRange.w);
    lightPos = light.positionRange.xyz;
    float3 toLight = lightPos - P;
    float dist = length(toLight);
    if (range > 0.0 && dist > range + 0.001) return false;
    L = toLight / max(dist, 1e-5);
    float d = saturate(1.0 - dist / range);
    Li = light.colorIntensity.rgb * light.colorIntensity.w * (d * d);
    return true;
}

// ============================================================
// 坐标变换
// ============================================================

// 世界坐标 → 屏幕 UV（GLM y-up 投影 → Vulkan UV，y 翻转）
float2 TransformWorldToUV(float3 worldPos, float4x4 viewProj) {
    float4 clip = mul(viewProj, float4(worldPos, 1.0));
    float3 ndc = clip.xyz / clip.w;
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

// 线性视图深度 → 世界坐标（透视相机：cameraPos + 反投影方向 * depth）
float3 ReconstructWorldPos(float2 uv, float depth, float4x4 invViewProj, float3 cameraPos) {
    float2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;  // Vulkan NDC y 翻转
    float4 clipPos = float4(ndc, 1.0, 1.0);
    float4 worldPos4 = mul(invViewProj, clipPos);
    float3 dir = normalize(worldPos4.xyz / worldPos4.w - cameraPos);
    return cameraPos + dir * depth;
}

#endif // HUGENGINE_PT_COMMON_SLANG
