// SHCommon.slang — 球谐（Spherical Harmonics）共享实现：l ≤ 1 的 4 系数表示
//
// 【为什么集中到这一个文件】同一组常数与重建公式此前在 4 处各写一份，任意一处改动
//（阶数 / 轴序 / 卷积系数 / 采样方向）都会让其余几处静默不一致：
//   · `GI/DDGI.comp.slang`                投影基函数 + Screen Probe 辐射度求值
//   · `RT_DDGI.slang`                     探针辐照度求值（DDGI 合成段与 RTGI miss 回退都走它）
//   · `Lumen/ScreenProbeSampling.slang`   投影基函数 + 辐射度→辐照度
//   · `RayTracing/DDGI_Trace.rgen.slang`  Fibonacci 采样方向（原注释要求"必须逐位一致"）
// 现在这些定义只在本文件出现一次。**改这里等于同时改所有使用者** —— 这是有意的：它让
// "投影端与评估端必须同约定"这条不变量不再依赖人工对齐（与"绑定号无自动对账"同类，见
// 《04.功能缺口与对标分析》§16）。
//
// 【约定】Ramamoorthi & Hanrahan 2001（实球谐，正交归一）：
//   Y00 = 0.5·√(1/π) ≈ 0.2820948      l = 0
//   Y1  = 0.5·√(3/π) ≈ 0.4886025      l = 1 的公共归一化常数
//   轴序：i = 1, 2, 3 依次对应 (y, z, x)，即 m = (-1, 0, 1) —— **不是** (x, y, z)。
//   辐照度重建（Lambert / clamped-cosine 卷积）：
//       E(n) = Â0·l0·Y00 + Â1·(l1·Y1(n) + l2·Y2(n) + l3·Y3(n)),   Â0 = π, Â1 = 2π/3
//
// 【为什么只到 l = 1（4 系数）】band 2（5 个系数）在步骤 30 被移除：Radiance Cache 的表示
//   统一为二阶 4 系数。保留这段说明是为了让"为什么少了一档"在代码里可见。若将来恢复 band 2，
//   Â2 = π/4，且**投影端与评估端必须同时改**（只改一边会让少的那一档变成静默偏移）。
//
// 【两种存储布局都支持】（本文件不强制其一，打包/解包在各调用点完成）
//   · 系数主序 `float4 sh[4]`：sh[i].rgb = 第 i 个系数的 RGB —— DDGI 探针缓冲
//   · 通道主序 `float4 shR/shG/shB`：每个通道 4 个系数 —— Lumen ScreenProbe
//
// 【白炉（L ≡ 1）解析判据】半球均匀采样投影 ⇒ l0 = 2π·Y00 = √π ≈ 1.7724539，且 E(n) ≡ π，
//   **与采样方向、采样数无关**。`Lumen/Lumen_ScreenProbe_SHProject.comp.slang` 用它做自检；
//   CPU 侧镜像常量见 `Engine/Render/Lumen/LumenSH.h`（kSHWhiteFurnaceL0）。
//
// 【数值约定】下面一律用**完整精度字面量**。此前几处拷贝写的是 10 位截断形式（如
//   0.2820947918），与完整精度在 fp32 下**舍入到同一个 float**（差值 ~2.6e-11，远小于
//   0.28 附近的 fp32 间隔 3e-8），故统一字面量在 shader 侧是数值无变化的。
#ifndef HUGENGINE_SH_COMMON
#define HUGENGINE_SH_COMMON

// ---- 基函数常数（实球谐，正交归一）----
static const float kSH_Y00 = 0.28209479177387814;   // 0.5·√(1/π)
static const float kSH_Y1  = 0.4886025119029199;    // 0.5·√(3/π)

// ---- Lambert 余弦卷积系数 Â_l = 2π·∫₀¹ t·P_l(t)dt ----
static const float kSH_A0 = 3.14159265358979323846;   // π
static const float kSH_A1 = 2.09439510239319549231;   // 2π/3
static const float kSH_A2 = 0.78539816339744830961;   // π/4（band 2 已移除，保留备查）

// ---- 预乘形式：Â_l 与 Y_lm 归一化常数的乘积（评估端最省算力）----
//   kSH_A0Y00 = Â0·Y00 = π·√(1/4π) = √π/2 ≈ 0.8862269255
//   kSH_A1Y1  = Â1·Y1  = (2π/3)·√(3/4π)      ≈ 1.0233267079
static const float kSH_A0Y00 = 0.8862269254527580;
static const float kSH_A1Y1  = 1.0233267079464886;

static const uint kSHCoeffCount = 4;   // l ≤ 1 ⇒ (1+1)² = 4 个系数

// ---- 基函数 ----

// 第 i 个基函数在方向 d 上的值（i ∈ [0,4)）。d 必须是**单位向量**。
float SHBasis(uint i, float3 d) {
    if (i == 0u) return kSH_Y00;
    if (i == 1u) return kSH_Y1 * d.y;   // m = -1
    if (i == 2u) return kSH_Y1 * d.z;   // m =  0
    return kSH_Y1 * d.x;                // m = +1（i == 3）
}

// 一次取齐 4 个基函数值（投影端累加用：sh[j] += L·basis[j]），避免在投影循环里逐次分支。
void SHBasis(float3 d, out float basis[4]) {
    basis[0] = kSH_Y00;
    const float3 b1 = d * kSH_Y1;
    basis[1] = b1.y; basis[2] = b1.z; basis[3] = b1.x;   // l=1: m=-1,0,1
}

// ---- 重建 ----

// 由 4 系数**辐射度** SH 在方向 d 上求值（**不做**余弦卷积）⇒ 结果仍是辐射度 L(d)。
float3 SHEvalRadiance(float3 l0, float3 l1, float3 l2, float3 l3, float3 d) {
    return l0 * kSH_Y00 + (l1 * d.y + l2 * d.z + l3 * d.x) * kSH_Y1;
}

// 由 4 系数**辐射度** SH 重建**辐照度**（施加 clamped-cosine 卷积 Â_l）。白炉下返回 π。
// 【为什么不做负值截断】截断与否是调用方的语义选择，不属共享实现：DDGI 探针求值需要截断
//（负辐照度无物理意义），而 Lumen 的诊断路径要保留原值才能统计偏差。
float3 SHRadianceToIrradiance(float3 l0, float3 l1, float3 l2, float3 l3, float3 n) {
    return l0 * kSH_A0Y00
         + (l1 * n.y + l2 * n.z + l3 * n.x) * kSH_A1Y1;
}

// ---- 采样方向（SH 投影的积分点）----

// Fibonacci 球面：单位球面上第 i 个方向（共 n 个），面积近似均匀。
//   极角按 (i+0.5)/n 偏移（沿 cosθ 均匀、避开两极点退化）；方位角按黄金角 π(1+√5) 递增。
// ⚠ 该函数被"投影端"（DDGI.comp / DDGI_Trace 共用采样方向）与 Lumen 侧共同依赖：
//   两侧必须取到同一组方向，否则同一条采样在不同路径下不是同一个点。集中到本文件后
//   这一条由构造保证，不再依赖"两份拷贝必须逐位一致"的注释约定。
float3 FibonacciSphere(int i, int n) {
    float phi   = acos(1.0 - 2.0 * (float(i) + 0.5) / float(n));
    float theta = 3.14159265359 * (1.0 + sqrt(5.0)) * float(i);
    return float3(sin(phi) * cos(theta), cos(phi), sin(phi) * sin(theta));
}

#endif // HUGENGINE_SH_COMMON
