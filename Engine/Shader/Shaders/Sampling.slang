// ============================================================
// Sampling.slang — 采样工具（**零绑定、零 include**）
//
// 提供：
//   · CosineSampleHemisphere — 余弦权重半球采样（PT 与 RT 共用；此前 PT_Common 与
//     RT_AO/RT_GI 各有一份逐字相同的实现）
//   · RadicalInverse_VdC / Hammersley — 低差异序列（IBL 的 BRDF LUT 与预滤波各一份）
//   · SampleGgxHalf / SampleGgxHalfAniso / SampleGgxReflection — GGX 半向量与反射方向采样
//     （PT / RT 反射 / Lumen 三处曾各写一份，现合并；IBL 烘焙端的 y 参考轴版本仍留在原文件）
// ============================================================
#ifndef HUGENGINE_SAMPLING_SLANG
#define HUGENGINE_SAMPLING_SLANG

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


float2 Hammersley(uint i, uint N) {
    return float2(float(i) / float(N), RadicalInverse_VdC(i));
}


float RadicalInverse_VdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

// ============================================================
// GGX 半向量 / 反射方向采样（各向同性 + 各向异性）
//
// 【为什么收敛】同一件事此前有 3 份不同名的实现：`PT_Common.SampleGGXHalf`、
// `RT_Reflection.rgen.SampleGGX`、`ScreenProbeSampling.SampleGgxDirection`。三者用的是**同一套
// 切线基**（`abs(N.z) < 0.999` 取 (0,0,1) 参考轴），只有 alpha 下限与返回值不同 ⇒ 现在合并为
// 下面 4 个入口，下限作为参数保留（各调用点原值不变，故数值逐位一致）。
//
// 【与 IBL 烘焙端的区别（不要互相替换）】`GI/IBL_BRDF_LUT.frag.slang` 与
// `GI/IBL_Prefilter.frag.slang` 各自实现的 `ImportanceSampleGGX` 用的是**y 参考轴**
// （`abs(N.y) < 0.999` ⇒ up=(0,1,0)）的基：分布相同，但给定 u 得到的朝向不同 ⇒ 替换会改变
// 烘焙结果。合并它们需要先决定"是否接受 IBL LUT/预滤波重新烘焙"，故本轮不做。
// ============================================================

// GGX 半向量采样（绕 N，z 参考轴基）。minAlpha = alpha 下限（各调用点沿用各自历史值）。
// NdotH 回出 cosθ_h，供 pdf 计算复用（避免调用点重算不一致）。
// 【参数顺序】minAlpha 必须排在 out NdotH **之前**：否则 4 参数调用会在
//   `(N,r,u,minAlpha)` 与 `(N,r,u,out NdotH)` 之间产生重载歧义（Slang error 39999）。
float3 SampleGgxHalf(float3 N, float roughness, float2 u, float minAlpha,
                     out float NdotH) {
    float alpha = max(roughness * roughness, minAlpha);
    // 分母保护 1e-6：仅在 alpha < 1e-3（极端尾部）时才可能生效，正常材质粗糙度下是空操作
    float cosTheta = sqrt((1.0 - u.y) / max(1.0 + (alpha * alpha - 1.0) * u.y, 1e-6));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float phi = 6.28318530718 * u.x;
    float3 H = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
    float3 Up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T = normalize(cross(Up, N));
    float3 B = cross(N, T);
    NdotH = cosTheta;
    return normalize(H.x * T + H.y * B + H.z * N);
}

// 不需要 NdotH 的重载（RT 反射直接取半向量做 reflect）——4 参数调用即命中此版本
float3 SampleGgxHalf(float3 N, float roughness, float2 u, float minAlpha = 0.001) {
    float NdotH;
    return SampleGgxHalf(N, roughness, u, minAlpha, NdotH);
}

// 各向异性 GGX 半向量采样（ax/ay 为沿 T/B 的 alpha）。
// aniso=0（默认材质）时调用方应走 SampleGgxHalf，采样与 pdf 与旧实现逐位一致。
float3 SampleGgxHalfAniso(float3 N, float3 T, float3 B,
                          float ax, float ay, float2 u, out float NdotH) {
    float phi    = 6.2831853 * u.x;
    float cosPhi = cos(phi), sinPhi = sin(phi);
    float a2     = ax * ax * cosPhi * cosPhi + ay * ay * sinPhi * sinPhi;
    float cosTheta = sqrt((1.0 - u.y) / max(1.0 + (a2 - 1.0) * u.y, 1e-6));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    NdotH = cosTheta;
    return normalize(sinTheta * cosPhi * T + sinTheta * sinPhi * B + cosTheta * N);
}

// 以 N 为入射方向（V = N）的 GGX **反射方向**采样：先取半向量再镜面反射。
// 供 Lumen 探针方向采样使用（原先 ScreenProbeSampling.SampleGgxDirection）。
float3 SampleGgxReflection(float3 N, float roughness, float2 u, float minAlpha = 0.002) {
    float3 H = SampleGgxHalf(N, roughness, u, minAlpha);
    return normalize(reflect(-N, H));
}

#endif // HUGENGINE_SAMPLING_SLANG
