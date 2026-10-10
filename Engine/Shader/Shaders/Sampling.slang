// ============================================================
// Sampling.slang — 采样工具（**零绑定、零 include**）
//
// 提供：
//   · CosineSampleHemisphere — 余弦权重半球采样（PT 与 RT 共用；此前 PT_Common 与
//     RT_AO/RT_GI 各有一份逐字相同的实现）
//   · RadicalInverse_VdC / Hammersley — 低差异序列（IBL 的 BRDF LUT 与预滤波各一份）
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

#endif // HUGENGINE_SAMPLING_SLANG
