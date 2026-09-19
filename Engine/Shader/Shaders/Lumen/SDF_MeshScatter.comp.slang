// SDF_MeshScatter.comp.slang — 逐 mesh 距离场构建（scatter 版，支撑 128³）
//
// 【与首版 gather 的关系】gather 是"每体素遍历全部三角形"，代价 O(体素 × 三角形)：128³ 下
// 2M 体素 × 数千三角形 = 10^9~10^10 次距离求值，不可接受。scatter 反过来：**每个三角形一个
// 线程组**，只更新它 AABB 内的体素，代价 O(表面面积 × 分辨率²)，与三角形数线性、与体素数无关。
// 这正对应《Lumen设计与实现》§5 写的"Compute Shader（Brute-force 每三角形写入体素）"。
//
// 【原子最小】距离非负，浮点位模式与无符号整数**同序**，故 `InterlockedMin(asuint(d))` 得到
// 精确的最小距离。清空（mode 0）写 0x7F800000 = +inf。
//
// 【符号】本版输出**无符号**距离。符号（parity / 最近三角形法线）与开放曲面的问题见 §5：
// 实测它对命中精度没有帮助（场景多为开放曲面，内外本身无定义），故 scatter 版不再计算符号。

[[vk::binding(0, 0)]] RWTexture3D<uint>         u_Field;
[[vk::binding(1, 0)]] StructuredBuffer<float4>  u_Positions;   // xyz = 位置
[[vk::binding(2, 0)]] StructuredBuffer<uint>    u_Indices;

[[vk::push_constant]] cbuffer ScatterPC {
    float4 originVoxel;   // xyz = 网格原点（mesh 局部空间）, w = 体素边长
    uint4  dims;          // xyz = 分辨率, w = mode（0=清空, 1=scatter）
    uint4  ranges;        // x = 三角形数, y = 索引起始, z = 顶点偏移, w = 未用
};

float DistancePointTriangle(float3 p, float3 a, float3 b, float3 c) {
    float3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) return length(ap);
    float3 bp = p - b;
    float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) return length(bp);
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) { float v = d1 / (d1 - d3); return length(p - (a + v * ab)); }
    float3 cp = p - c;
    float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) return length(cp);
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) { float w = d2 / (d2 - d6); return length(p - (a + w * ac)); }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return length(p - (b + w * (c - b)));
    }
    float denom = va + vb + vc;
    if (abs(denom) < 1e-12) return length(ap);
    float v = vb / denom, w = vc / denom;
    return length(p - (a + ab * v + ac * w));
}

static const uint kThreads = 64;

[numthreads(kThreads, 1, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    if (dims.w == 0u) {
        // ── mode 0：清空为 +inf（一维线性下标，组数 = ceil(res³ / 64)）──
        const uint total = dims.x * dims.y * dims.z;
        for (uint i = gid.x * kThreads + tid.x; i < total; i += kThreads) {
            const uint x = i % dims.x;
            const uint y = (i / dims.x) % dims.y;
            const uint z = i / (dims.x * dims.y);
            u_Field[uint3(x, y, z)] = 0x7F800000u;
        }
        return;
    }

    // ── mode 1：一个三角形一个线程组，只扫它自己的 AABB ──
    const uint tri = gid.x;
    if (tri >= ranges.x) return;

    const uint base = ranges.y + tri * 3u;
    const float3 a = u_Positions[u_Indices[base + 0u] + ranges.z].xyz;
    const float3 b = u_Positions[u_Indices[base + 1u] + ranges.z].xyz;
    const float3 c = u_Positions[u_Indices[base + 2u] + ranges.z].xyz;

    const float3 lo = min(min(a, b), c);
    const float3 hi = max(max(a, b), c);
    const int3 vlo = clamp(int3(floor((lo - originVoxel.xyz) / originVoxel.w)) - 1,
                           int3(0), int3(dims.xyz) - 1);
    const int3 vhi = clamp(int3(ceil((hi - originVoxel.xyz) / originVoxel.w)) + 1,
                           int3(0), int3(dims.xyz) - 1);
    const int3 span = vhi - vlo + 1;
    if (any(span <= 0)) return;

    const uint total = (uint)(span.x * span.y * span.z);
    const uint sx = (uint)span.x, sy = (uint)span.y;
    for (uint i = tid.x; i < total; i += kThreads) {
        const int3 off = int3(i % sx, (i / sx) % sy, i / (sx * sy));
        const int3 v = vlo + off;
        const float3 p = originVoxel.xyz + (float3(v) + 0.5) * originVoxel.w;
        const float d = DistancePointTriangle(p, a, b, c);
        InterlockedMin(u_Field[uint3(v)], asuint(d));
    }
}
