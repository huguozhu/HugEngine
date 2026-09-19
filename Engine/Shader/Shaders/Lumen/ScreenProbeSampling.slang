// ScreenProbeSampling.slang — 探针半球采样与 SH 基（步骤 21 / 23 共享，**Slang 专用**）
//
// 【为什么必须共享】步骤 23 的 SH 投影要把"每探针 traceRep 条光线"投成系数，而光线只有方向和
// 结果缓冲（没有方向缓冲）。若两个 pass 各写一份采样代码，任何一处改动都会让"投影用的方向"与
// "追踪用的方向"悄悄错开 —— 那是最难查的一类错误（系数看起来正常，却是别的方向的积分）。
// 因此采样与随机数只在这里定义一次，追踪 pass 与投影 pass 各自 include。
//
// 【采样模式】
//   0 = GGX 半向量采样（步骤 21 原样，反射式镜面分布）
//   1 = **均匀半球采样**（步骤 23 起为默认）：pdf = 1/(2π)，于是 SH 投影估计量
//       `l_i = (2π/N) Σ L_j Y_i(d_j)` 的 0 阶项在白炉（L ≡ 1）下等于 `2π·Y00 = √π`，
//       与采样方向、采样数**无关** ⇒ 步骤 23 的验收"白炉下误差为 0"可以被数值钉死。
//       （余弦采样虽方差更低，但权重是 π/cosθ，白炉下 0 阶项不再有解析值。）
#ifndef HUGENGINE_SCREEN_PROBE_SAMPLING
#define HUGENGINE_SCREEN_PROBE_SAMPLING

// 确定性的低差异随机（PCG 变体）：同一 (探针, 光线, 帧) 恒得同一方向 ⇒ 可复现（背靠背读数一致的前提）
uint Pcg(uint v) {
    v = v * 747796405u + 2891336453u;
    const uint w = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return (w >> 22u) ^ w;
}
float U01(uint seed) { return (float)(Pcg(seed) & 0x00FFFFFFu) / 16777216.0; }

/// 以 n 为 Z 轴的正交基（避免 abs(n.z) ≈ 1 时叉乘退化）
void BuildBasis(float3 n, out float3 t, out float3 b) {
    t = normalize(abs(n.z) < 0.999 ? cross(float3(0.0, 0.0, 1.0), n) : cross(float3(1.0, 0.0, 0.0), n));
    b = cross(n, t);
}

/// 以 n 为 Z 轴的 GGX 半向量采样，再镜面反射得到出射方向（V = n）
float3 SampleGgxDirection(float3 n, float roughness, float2 u) {
    const float a = max(0.002, roughness * roughness);
    const float phi = 6.28318530718 * u.x;
    const float cosTheta = sqrt((1.0 - u.y) / max(1e-6, 1.0 + (a * a - 1.0) * u.y));
    const float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float3 t, b;
    BuildBasis(n, t, b);
    const float3 h = t * (sinTheta * cos(phi)) + b * (sinTheta * sin(phi)) + n * cosTheta;
    return normalize(reflect(-n, h));
}

/// 以 n 为 Z 轴的**均匀半球**采样（pdf = 1/(2π)）：cosθ = u.y 均匀 ⇒ 面积均匀
float3 SampleUniformHemisphere(float3 n, float2 u) {
    const float phi = 6.28318530718 * u.x;
    const float cosTheta = max(0.0, u.y);
    const float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float3 t, b;
    BuildBasis(n, t, b);
    return normalize(t * (sinTheta * cos(phi)) + b * (sinTheta * sin(phi)) + n * cosTheta);
}

/// 同一 (探针, 光线, 帧) 的两个 [0,1) 随机数（追踪与投影必须完全一致地取到）
float2 ProbeRayRandom(uint rayFlatIndex, uint frameSeed) {
    return float2(U01(rayFlatIndex * 3u + frameSeed * 7919u + 1u),
                  U01(rayFlatIndex * 7u + frameSeed * 6271u + 5u));
}

// ── 二阶（4 系数）SH 基：Y00 常数 + 3 个线性项 ──
// 常数与 Ramamoorthi & Hanrahan 的约定一致（Y00 = 0.5·√(1/π)、Y1 = 0.5·√(3/π)）。
static const float kSH_Y00 = 0.2820947918;   // 0.5·sqrt(1/π)
static const float kSH_Y1  = 0.4886025119;   // 0.5·sqrt(3/π)

/// 第 i 个（0..3）基函数在方向 d 上的值
float SHBasis(uint i, float3 d) {
    if (i == 0u) return kSH_Y00;
    if (i == 1u) return kSH_Y1 * d.y;
    if (i == 2u) return kSH_Y1 * d.z;
    return kSH_Y1 * d.x;
}

/// 由**辐射度**的 4 系数重建**辐照度**：E(n) = π·l0·Y00 + (2π/3)·(l1·Y1(n) + l2·Y2(n) + l3·Y3(n))
/// （Â0 = π、Â1 = 2π/3 是"clamped cosine 卷积"的解析系数；白炉下它给出 E ≡ π）
float3 SHRadianceToIrradiance(float3 l0, float3 l1, float3 l2, float3 l3, float3 n) {
    const float A0 = 3.14159265359;
    const float A1 = 2.09439510239;   // 2π/3
    return l0 * (A0 * kSH_Y00)
         + (l1 * (kSH_Y1 * n.y) + l2 * (kSH_Y1 * n.z) + l3 * (kSH_Y1 * n.x)) * A1;
}

#endif // HUGENGINE_SCREEN_PROBE_SAMPLING
