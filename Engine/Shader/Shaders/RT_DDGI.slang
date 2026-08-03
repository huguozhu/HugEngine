// RT_DDGI.slang — DDGI 探针查询共享库（纯函数 + 常量，无资源绑定）
// 由 DeferredLighting.frag（binding 22）与 RT_GI.rgen（binding 7）各自声明
// [[vk::binding(N, 0)]] StructuredBuffer<float4> u_DDGIProbes 后 include 本文件使用。

// ---- DDGI 探针网格参数（TODO: 改为 Uniform Buffer，当前与 GI_DDGI 默认值保持同步） ----
static const uint   kDDGI_GridX    = 8;
static const uint   kDDGI_GridY    = 4;
static const uint   kDDGI_GridZ    = 8;
static const float  kDDGI_CellSize = 3.0;
static const float3 kDDGI_Origin   = float3(-10.0, -2.0, -10.0);
static const uint   kDDGI_Stride   = 16;  // 每探针 16 float4

// SH 基函数常量
static const float kDDGI_SH_Y00 = 0.28209479177387814;       // l=0: sqrt(1/(4π))
static const float kDDGI_SH_B1  = 0.4886025119029199;        // l=1: sqrt(3/(4π))

// 在方向 dir 处评估 9 系数 SH 的辐照度（bands 0/1/2 完整版）
float3 EvalDDGI_SH(float4 sh[9], float3 dir) {
    float3 result = sh[0].rgb * kDDGI_SH_Y00;                // l=0, m=0
    result += sh[1].rgb * dir.y * kDDGI_SH_B1;               // l=1, m=-1
    result += sh[2].rgb * dir.z * kDDGI_SH_B1;               // l=1, m=0
    result += sh[3].rgb * dir.x * kDDGI_SH_B1;               // l=1, m=1
    // l=2, m=-2: sqrt(15/(4π)) * x*y
    result += sh[4].rgb * 1.0925484305920792 * dir.x * dir.y;
    // l=2, m=-1: sqrt(15/(4π)) * y*z
    result += sh[5].rgb * 1.0925484305920792 * dir.y * dir.z;
    // l=2, m=0:  sqrt(5/(16π)) * (3z²-1)
    result += sh[6].rgb * 0.31539156525252005 * (3.0 * dir.z * dir.z - 1.0);
    // l=2, m=1:  sqrt(15/(4π)) * x*z
    result += sh[7].rgb * 1.0925484305920792 * dir.x * dir.z;
    // l=2, m=2:  sqrt(15/(16π)) * (x²-y²)
    result += sh[8].rgb * 0.5462742152960396 * (dir.x * dir.x - dir.y * dir.y);
    return max(result, 0.0);
}

// 三线性插值采样 DDGI 探针网格，返回间接漫反射辐照度
float3 SampleDDGI(float3 worldPos, float3 normal) {
    // 世界坐标 → 探针网格坐标
    float3 gridCoord = (worldPos - kDDGI_Origin) / kDDGI_CellSize;
    int3   baseCoord = int3(floor(gridCoord));
    float3 frac      = frac(gridCoord);

    // 钳制到网格边界内
    baseCoord = clamp(baseCoord, int3(0, 0, 0),
                      int3(kDDGI_GridX - 1, kDDGI_GridY - 1, kDDGI_GridZ - 1));

    float3 result      = float3(0.0, 0.0, 0.0);
    float  totalWeight = 0.0;

    // 遍历包围世界坐标的 8 个最近探针
    for (int dz = 0; dz <= 1; dz++) {
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = 0; dx <= 1; dx++) {
                int3 coord = baseCoord + int3(dx, dy, dz);
                coord = clamp(coord, int3(0, 0, 0),
                              int3(kDDGI_GridX - 1, kDDGI_GridY - 1, kDDGI_GridZ - 1));

                // 三线性插值权重
                float weight = (1.0 - abs(float(dx) - frac.x))
                             * (1.0 - abs(float(dy) - frac.y))
                             * (1.0 - abs(float(dz) - frac.z));

                // 读取探针 SH 系数（每探针 16 × float4，前 9 个是 SH）
                uint   baseIdx = coord.z * kDDGI_GridX * kDDGI_GridY
                               + coord.y * kDDGI_GridX + coord.x;
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
