// RT_DDGI.slang — DDGI 探针查询共享库（纯函数，无资源绑定）
// 由 DeferredLighting.frag（binding 22）与 RT_GI.rgen（binding 7）各自声明
// [[vk::binding(N, 0)]] StructuredBuffer<float4> u_DDGIProbes 后 include 本文件使用。
// 此外 includer 需在 include 之前声明 DDGI 网格参数 cbuffer
// （u_DDGIGridOrigin / u_DDGIGridSize），供 SampleDDGI 三线性插值使用。

// ---- DDGI 探针网格参数 ----
// 网格参数（gridX/gridY/gridZ/cellSize/origin）为运行时字段，由 includer 声明为 cbuffer
// （u_DDGIGridOrigin / u_DDGIGridSize），与 GI_DDGI 的 ProbeGridUniform 前两个 float4 一致，
// 避免 C++ 侧改网格后 shader 硬编码常量不同步。
static const uint   kDDGI_Stride   = 16;  // 每探针 16 float4（布局不变量，与 C++ kFloats4PerProbe 一致）

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
static const float kDDGI_SH_A2a = 0.8580855308097856;  // A_2·sqrt(15/(4π))
static const float kDDGI_SH_A2b = 0.2477080863724551;  // A_2·sqrt(5/(16π))
static const float kDDGI_SH_A2c = 0.4290427654048928;  // A_2·sqrt(15/(16π))

// 在方向 dir 处评估 9 系数 SH 的辐照度（bands 0/1/2 完整版，含 Lambert 卷积）
float3 EvalDDGI_SH(float4 sh[9], float3 dir) {
    float3 result = sh[0].rgb * kDDGI_SH_A0;                 // l=0, m=0
    result += sh[1].rgb * dir.y * kDDGI_SH_A1;               // l=1, m=-1
    result += sh[2].rgb * dir.z * kDDGI_SH_A1;               // l=1, m=0
    result += sh[3].rgb * dir.x * kDDGI_SH_A1;               // l=1, m=1
    // l=2, m=-2: sqrt(15/(4π)) * x*y
    result += sh[4].rgb * kDDGI_SH_A2a * dir.x * dir.y;
    // l=2, m=-1: sqrt(15/(4π)) * y*z
    result += sh[5].rgb * kDDGI_SH_A2a * dir.y * dir.z;
    // l=2, m=0:  sqrt(5/(16π)) * (3z²-1)
    result += sh[6].rgb * kDDGI_SH_A2b * (3.0 * dir.z * dir.z - 1.0);
    // l=2, m=1:  sqrt(15/(4π)) * x*z
    result += sh[7].rgb * kDDGI_SH_A2a * dir.x * dir.z;
    // l=2, m=2:  sqrt(15/(16π)) * (x²-y²)
    result += sh[8].rgb * kDDGI_SH_A2c * (dir.x * dir.x - dir.y * dir.y);
    // 负值截断：辐照度物理上非负；SH 用 9 系数（l≤2）表示余弦波瓣时会有轻微振铃，
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

                // 读取探针 SH 系数（每探针 16 × float4，前 9 个是 SH）
                uint   baseIdx = coord.z * uint(u_DDGIGridSize.x) * uint(u_DDGIGridSize.y)
                               + coord.y * uint(u_DDGIGridSize.x) + coord.x;
                uint   bufBase = baseIdx * kDDGI_Stride;
                float4 sh[9];
                for (int i = 0; i < 9; i++) {
                    sh[i] = u_DDGIProbes[bufBase + i];
                }

                result      += EvalDDGI_SH(sh, normal) * weight;
                totalWeight += weight;
            }
        }
    }

    return result / max(totalWeight, 0.001);
}
