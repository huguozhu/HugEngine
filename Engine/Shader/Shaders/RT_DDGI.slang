// RT_DDGI.slang — DDGI 探针查询共享库（纯函数，无资源绑定）
// 由 DeferredLighting.frag（binding 22）与 RT_GI.rgen（binding 7）各自声明
// [[vk::binding(N, 0)]] StructuredBuffer<float4> u_DDGIProbes 后 include 本文件使用。
// 此外 includer 需在 include 之前声明 DDGI 网格参数 cbuffer
// （u_DDGIGridOrigin / u_DDGIGridSize），供 SampleDDGI 三线性插值使用。

// ---- DDGI 探针网格参数 ----
// 网格参数（gridX/gridY/gridZ/cellSize/origin）为运行时字段，由 includer 声明为 cbuffer
// （u_DDGIGridOrigin / u_DDGIGridSize），与 GI_DDGI 的 ProbeGridUniform 前两个 float4 一致，
// 避免 C++ 侧改网格后 shader 硬编码常量不同步。
static const uint   kDDGI_Stride   = 4;   // 每探针 **4** float4（步骤 30：二阶 4 系数；与 C++ kFloats4PerProbe 一致）

// ── SH 基函数常量 ──
static const float kDDGI_SH_Y00 = 0.28209479177387814;       // l=0: sqrt(1/(4π))
static const float kDDGI_SH_B1  = 0.4886025119029199;        // l=1: sqrt(3/(4π))

// ── Lambert 余弦波瓣的 SH 卷积系数（M5.3）──
// 探针投影得到的是**辐射度** SH（L_lm = 4π/N·Σ L·Y，不含 cos）；
// 要得到「辐照度」E(n) = ∫L(ω)·max(0, n·ω)dω，必须在**评估端**乘 SH 卷积系数：
//     E(n) = Σ_l A_l · L_lm · Y_lm(n)
//   A_0 = π,  A_1 = 2π/3,  A_2 = π/4      (Ramamoorthi & Hanrahan 2001)
// 注意：不能在「投影时乘 cos」——cos 依赖评估方向（法线），而探针存储时方向未知；
//       卷积只能在评估端以与 n 无关的 A_l 形式应用。
// 下方常量即 A_l 与各 Y_lm 归一化常数的乘积：
static const float kDDGI_SH_A0 = 0.8862269254527580;   // A_0·Y00 = π·sqrt(1/4π)
static const float kDDGI_SH_A1 = 1.0233267079464886;   // A_1·B1  = (2π/3)·sqrt(3/4π)
// 【步骤 30】band2 的 A_2 常数与 5 个 l=2 项一并移除：Radiance Cache 的表示统一为二阶 4 系数
//（§7）。投影端（`DDGI.comp.slang`）必须**同时**改 —— 只改一边会让"少的那一档"变成静默偏移。

// 在方向 dir 处评估 **4** 系数 SH 的辐照度（bands 0/1，含 Lambert 卷积）
float3 EvalDDGI_SH(float4 sh[4], float3 dir) {
    float3 result = sh[0].rgb * kDDGI_SH_A0;                 // l=0, m=0
    result += sh[1].rgb * dir.y * kDDGI_SH_A1;               // l=1, m=-1
    result += sh[2].rgb * dir.z * kDDGI_SH_A1;               // l=1, m=0
    result += sh[3].rgb * dir.x * kDDGI_SH_A1;               // l=1, m=1
    // 负值截断：辐照度物理上非负；SH 表示余弦波瓣时仍可能有轻微振铃（二阶更明显），
    // 截断避免负辐照度进入画面。若后续振铃明显，可改为 SH 窗口化（如 Hanning）平滑抑制。
    return max(result, 0.0);
}

// 三线性插值采样 DDGI 探针网格，返回间接漫反射辐照度
float3 SampleDDGI(float3 worldPos, float3 normal) {
    // 世界坐标 → 探针网格坐标（u_DDGIGridSize.w = cellSize）
    float3 gridCoord = (worldPos - u_DDGIGridOrigin.xyz) / u_DDGIGridSize.w;
    int3   baseCoord = int3(floor(gridCoord));
    float3 frac      = frac(gridCoord);

    // 钳制到网格边界内
    baseCoord = clamp(baseCoord, int3(0, 0, 0),
                      int3(int(u_DDGIGridSize.x) - 1,
                           int(u_DDGIGridSize.y) - 1,
                           int(u_DDGIGridSize.z) - 1));

    float3 result      = float3(0.0, 0.0, 0.0);
    float  totalWeight = 0.0;

    // 遍历包围世界坐标的 8 个最近探针
    for (int dz = 0; dz <= 1; dz++) {
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = 0; dx <= 1; dx++) {
                int3 coord = baseCoord + int3(dx, dy, dz);
                coord = clamp(coord, int3(0, 0, 0),
                              int3(int(u_DDGIGridSize.x) - 1,
                                   int(u_DDGIGridSize.y) - 1,
                                   int(u_DDGIGridSize.z) - 1));

                // 三线性插值权重
                float weight = (1.0 - abs(float(dx) - frac.x))
                             * (1.0 - abs(float(dy) - frac.y))
                             * (1.0 - abs(float(dz) - frac.z));

                // 读取探针 SH 系数（步骤 30：每探针 4 × float4，全是 SH）
                uint   baseIdx = coord.z * uint(u_DDGIGridSize.x) * uint(u_DDGIGridSize.y)
                               + coord.y * uint(u_DDGIGridSize.x) + coord.x;
                uint   bufBase = baseIdx * kDDGI_Stride;
                float4 sh[4];
                for (int i = 0; i < 4; i++) {
                    sh[i] = u_DDGIProbes[bufBase + i];
                }

                result      += EvalDDGI_SH(sh, normal) * weight;
                totalWeight += weight;
            }
        }
    }

    return result / max(totalWeight, 0.001);
}
