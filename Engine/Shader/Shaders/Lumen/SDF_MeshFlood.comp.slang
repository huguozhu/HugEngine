// SDF_MeshFlood.comp.slang — 跳步洪泛（jump flooding）：补全 scatter 留下的空洞
//
// 【为什么需要】scatter 只写"三角形 AABB 内"的体素 —— 也就是表面附近的一条带。远离表面的体素
// 仍是 +inf，而 +inf 在 sphere tracing 里是**高估**（会直接跳过几何）。跳步洪泛按 res/2, res/4 …
// 1 的步长逐级松弛：每个体素取"自身"与 26 个邻居（步长 s）的 `值 + 步长距离` 的最小值。
// 这与 UE 的 mesh DF 构建同一思路（scatter 后做 sweep/flood 补全场）。
//
// 【近似】洪泛以 L∞ 邻域松弛，得到的是网格采样的**近似**距离场（远处可能有不到一个体素的误差），
// 表面附近的精度仍来自 scatter 的精确点-三角形距离 —— 而 sphere tracing 关心的正是表面附近。

[[vk::binding(0, 0)]] RWTexture3D<uint> u_Field;

[[vk::push_constant]] cbuffer FloodPC {
    float4 originVoxel;   // xyz = 网格原点, w = 体素边长（把步长换算成距离）
    uint4  dims;          // xyz = 分辨率, w = 本次洪泛的步长（体素）
    uint4  ranges;        // 未用
};

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id >= dims.xyz)) return;
    const int s = (int)dims.w;
    if (s <= 0) return;

    const int3 size = int3(dims.xyz);
    float best = asfloat(u_Field[id]);
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0 && dz == 0) continue;
                const int3 n = int3(id) + int3(dx, dy, dz) * s;
                if (any(n < int3(0)) || any(n >= size)) continue;
                const float dn = asfloat(u_Field[uint3(n)]) +
                                 length(float3(dx, dy, dz)) * float(s) * originVoxel.w;
                best = min(best, dn);
            }
        }
    }
    u_Field[id] = asuint(best);
}
