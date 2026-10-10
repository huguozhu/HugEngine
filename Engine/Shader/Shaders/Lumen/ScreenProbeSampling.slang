// ScreenProbeSampling.slang — 探针半球采样（步骤 21 / 23 共享，**Slang 专用**）
//
// 【SH 基与重建不在这里】4 系数球谐的基函数、Lambert 卷积重建、白炉判据说明统一在
// `SHCommon.slang`（GI / RT / Lumen 三处共用同一份）。本文件只负责"方向怎么取"。
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

#include "Sampling.slang"   // SampleGgxReflection（GGX 反射方向，唯一出处）
#include "SHCommon.slang"   // 4 系数 SH 的基函数 / 重建（唯一出处；本文件不再自带常数）

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

// ── 二阶（4 系数）SH：基函数与辐照度重建都在 "SHCommon.slang" ──
// 本文件此前自带 kSH_Y00 / kSH_Y1 / SHBasis / SHRadianceToIrradiance 一份拷贝，
// 与 `GI/DDGI.comp.slang`、`RT_DDGI.slang` 重复；现统一到 SHCommon，改约定只需改一处。
// 使用方照旧 include 本文件即可（SHCommon 在本文件里已被 include）。

#endif // HUGENGINE_SCREEN_PROBE_SAMPLING
