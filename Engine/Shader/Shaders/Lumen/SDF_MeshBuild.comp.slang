// SDF_MeshBuild.comp.slang — 逐 mesh 距离场构建（步骤 8）
//
// 【首版为什么是 gather（每体素遍历全部三角形）而不是设计里的"每三角形写入体素"】
// 设计（§5）写的是"Compute Shader（Brute-force 每三角形写入体素）"，即 scatter + 原子最小。
// 那需要：u32 距离场 + InterlockedMin + 转换 pass，三趟与临时场，才能支撑 128³。
// 首版先用 gather 把**数据链路**（缓冲布局、网格映射、读回自检）验证正确，代价是
// O(体素 × 三角形)，因此配两个上限：分辨率（默认 32³）与每 mesh 三角形数。
// 步骤 10/11 真需要 128³ 时按 §12 的"实现前置"换成 scatter 版本（接口不变，只换 shader 与
// 加一张 r32ui 临时场）。
//
// 【量纲】输出是**无符号**距离（世界/局部空间单位）：本版不做内外符号判定。符号（inside/
// outside）与 sphere tracing 的步进一起在步骤 11 落地；在此之前它已足以"命中表面"。
//
// 【自检探针】每 stride 体素采一个点写进 u_ProbeDist（只对第 0 个 mesh），供 CPU 侧用同一
// 公式做独立复核 —— 体素映射/偏移/描述符这三处一旦错位，输出会静默变 0 或错位，画面看不出来。

[[vk::push_constant]] cbuffer BuildPC {
    float4 originVoxel;   // xyz = 网格原点（网格局部空间）, w = 体素边长
    uint4  dims;          // xyz = 分辨率, w = mesh 序号（0 = 需要写自检探针）
    uint4  ranges;        // x = 三角形数, y = 索引起始偏移, z = 顶点偏移, w = 自检采样步长
};

[[vk::binding(0, 0)]] RWTexture3D<float>        u_Field;
[[vk::binding(1, 0)]] StructuredBuffer<float4>  u_Positions;   // xyz = 位置（w 未用）
[[vk::binding(2, 0)]] StructuredBuffer<uint>    u_Indices;
[[vk::binding(3, 0)]] RWStructuredBuffer<float> u_ProbeDist;

/// 点到三角形的精确距离（Ericson, Real-Time Collision Detection 的闭式解）
float PointTriangleDistance(float3 p, float3 a, float3 b, float3 c) {
    float3 ab = b - a;
    float3 ac = c - a;
    float3 ap = p - a;
    float d1 = dot(ab, ap);
    float d2 = dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) return length(ap);          // 顶点 A 区域

    float3 bp = p - b;
    float d3 = dot(ab, bp);
    float d4 = dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) return length(bp);            // 顶点 B 区域

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {               // 边 AB
        float v = d1 / (d1 - d3);
        return length(p - (a + v * ab));
    }

    float3 cp = p - c;
    float d5 = dot(ab, cp);
    float d6 = dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) return length(cp);            // 顶点 C 区域

    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {               // 边 AC
        float w = d2 / (d2 - d6);
        return length(p - (a + w * ac));
    }

    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) { // 边 BC
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return length(p - (b + w * (c - b)));
    }

    float denom = va + vb + vc;
    if (abs(denom) < 1e-12) return length(ap);               // 退化三角形
    float v = vb / denom;
    float w = vc / denom;
    return length(p - (a + ab * v + ac * w));                // 面内
}

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id >= dims.xyz)) return;

    const float3 p = originVoxel.xyz + (float3(id) + 0.5) * originVoxel.w;
    float best = 1e30;
    uint  crossings = 0;      // 沿 +X 的穿越计数（奇 = 在几何内部）
    const uint triCount = ranges.x;
    for (uint t = 0; t < triCount; ++t) {
        const uint base = ranges.y + t * 3u;
        const float3 a = u_Positions[u_Indices[base + 0u] + ranges.z].xyz;
        const float3 b = u_Positions[u_Indices[base + 1u] + ranges.z].xyz;
        const float3 c = u_Positions[u_Indices[base + 2u] + ranges.z].xyz;
        best = min(best, PointTriangleDistance(p, a, b, c));

        // ── 符号：parity（沿 +X 射线与三角形求交，奇数个交点 = 内部）──
        // 在 YZ 平面做二维重心坐标，再解出交点的 X：比完整 Möller–Trumbore 少一半运算，
        // 且是本循环里顺手能算的。退化（投影面积≈0）跳过；边界/顶点上的抖动由 CPU 侧的
        // "最近三角形法线" 符号做交叉验证（两种算法，见 LumenSDF::RunSelfCheck）。
        const float2 pa = a.yz, pb = b.yz, pc = c.yz;
        const float2 pq = p.yz;
        const float  d2 = (pb.y - pc.y) * (pa.x - pc.x) + (pc.x - pb.x) * (pa.y - pc.y);
        if (abs(d2) > 1e-12) {
            const float l0 = ((pb.y - pc.y) * (pq.x - pc.x) + (pc.x - pb.x) * (pq.y - pc.y)) / d2;
            const float l1 = ((pc.y - pa.y) * (pq.x - pc.x) + (pa.x - pc.x) * (pq.y - pc.y)) / d2;
            const float l2 = 1.0 - l0 - l1;
            if (l0 >= 0.0 && l1 >= 0.0 && l2 >= 0.0) {
                const float xHit = l0 * a.x + l1 * b.x + l2 * c.x;
                if (xHit > p.x) crossings++;
            }
        }
    }
    const float signedDist = (crossings & 1u) ? -best : best;
    u_Field[id] = signedDist;

    // 自检探针（仅 mesh 0）
    const uint stride = ranges.w;
    if (dims.w == 0u && stride > 0u) {
        if ((id.x % stride) == 0u && (id.y % stride) == 0u && (id.z % stride) == 0u) {
            const uint nx = dims.x / stride, ny = dims.y / stride, nz = dims.z / stride;
            const uint idx = (id.z / stride) * nx * ny + (id.y / stride) * nx + (id.x / stride);
            if (idx < nx * ny * nz) u_ProbeDist[idx] = signedDist;
        }
    }
}
