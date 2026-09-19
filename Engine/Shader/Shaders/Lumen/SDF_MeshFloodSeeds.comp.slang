// SDF_MeshFloodSeeds.comp.slang — 带"种子坐标"的跳步洪泛（真正的 JFA）
//
// 【为什么不能再用"值 + 每跳步长"那版】那版的 fixpoint 是 **26 连通图上的最短路径长度**
// （每跳只把 |dir|*s*voxel 加到邻居值上），它恒 ≥ 真欧氏距离，最坏偏差约 8%（方向各向异性）。
// 实测 mesh 场在 2000 单位远处高估 +187（≈9.4%），正是这个偏差；而它又是"细场隧穿、
// 只能用 1 个体素容差兜住"的根因（见 §附八 / §附十三）。
//
// 本版按标准 JFA 存**最近种子的体素坐标**：每级只在 26 个邻居里比较"哪个种子离我最近"，
// 距离由种子坐标**精确**算出 ⇒ fixpoint 是真正的欧氏距离场（误差 ~1 体素，与传播距离无关）。
//
// 【与 scatter 的衔接】scatter 仍然用 u32 距离场做原子最小（浮点位模式同序）；本 pass 的
// `dims.w == 0` 是"种子初始化"：带内体素（距离有限）以**自己**为最近种子，带外标记无种子。
//
// 【坐标打包】种子坐标按 8 位/轴打包进一个 u32（分辨率 ≤ 256 时成立，C++ 侧有断言）。
[[vk::binding(0, 0)]] RWTexture3D<uint> u_Dist;   // 距离（浮点位模式）
[[vk::binding(1, 0)]] RWTexture3D<uint> u_Seed;   // 最近种子坐标（x | y<<8 | z<<16），0xFFFFFFFF = 无种子

[[vk::push_constant]] cbuffer FloodPC {
    float4 originVoxel;   // xyz = 网格原点, w = 体素边长
    uint4  dims;          // xyz = 分辨率, w = 步长（0 = 种子初始化）
    uint4  ranges;        // 未用
};

static const uint kNoSeed = 0xFFFFFFFFu;

uint PackSeed(int3 v) {
    return (uint(v.x) & 0xFFu) | ((uint(v.y) & 0xFFu) << 8) | ((uint(v.z) & 0xFFu) << 16);
}
int3 UnpackSeed(uint s) {
    return int3(int(s & 0xFFu), int((s >> 8) & 0xFFu), int((s >> 16) & 0xFFu));
}
// 距离 = **种子自己的距离 + 到种子的欧氏距离**（标准 vector-DT）：只用到种子的欧氏距离会
// 系统性**低估**（种子在 ±4 体素精确带上，离真实表面还有一小段），实测细场因此到处"立刻命中"。
// 种子自己那格的距离在 scatter 之后就不再变化（min-plus 不会把它改大），所以直接读 u_Dist[seed] 即可。
float SeedDistance(int3 v, uint seed) {
    const int3 sv = UnpackSeed(seed);
    return asfloat(u_Dist[uint3(sv)]) + length(float3(v - sv)) * originVoxel.w;
}

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id >= dims.xyz)) return;

    if (dims.w == 0u) {   // ── 种子初始化 ──
        u_Seed[id] = (asfloat(u_Dist[id]) < 1e30) ? PackSeed(int3(id)) : kNoSeed;
        return;
    }

    const int s = (int)dims.w;
    if (s <= 0) return;

    const int3 v = int3(id);
    uint  bestSeed = u_Seed[id];
    float best = (bestSeed == kNoSeed) ? 1e30 : SeedDistance(v, bestSeed);

    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0 && dz == 0) continue;
                const int3 n = v + int3(dx, dy, dz) * s;
                if (any(n < int3(0)) || any(n >= int3(dims.xyz))) continue;
                const uint ns = u_Seed[uint3(n)];
                if (ns == kNoSeed) continue;
                const float d = SeedDistance(v, ns);   // 用**种子坐标**算欧氏距离（不是加每跳步长）
                if (d < best) { best = d; bestSeed = ns; }
            }
        }
    }
    u_Dist[id] = asuint(best);
    u_Seed[id] = bestSeed;
}
