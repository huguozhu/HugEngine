// SDF_GlobalBuild.comp.slang — Global SDF 注入（步骤 10）
//
// 设计（《Lumen设计与实现》§5）：Mesh SDF →（本步）Global SDF 注入 →（步骤 11）sphere tracing。
// 三种模式用 push constant 的 dims.w 选择，避免为"清空/注入/转换"各写一个 shader：
//   mode 0 = clear：把全局 u32 场写成 +inf（浮点位模式 0x7F800000），供原子最小使用；
//   mode 1 = inject：把**一个** mesh 的距离场注入全局场（体素落在该 mesh AABB 内才写）；
//   mode 2 = convert：u32 → R32F 输出 + 写自检探针（供 CPU 侧独立复核）。
//
// 【已知近似（写进 §5 的质量边界）】注入用"取各 mesh 场的最小值"，每个 mesh 的场只在它自己的
// AABB 内有效。若某体素的真实最近面属于 mesh A，而该体素落在 A 的 AABB 之外（例如被更大的
// 相邻网格遮挡），全局值会**偏大**（下界不成立）。UE 的 Global DF 也是同样的 min 合并思路，
// 并用 Detail Tracing（前 2 m 走 mesh 场）来补精度。本版用自检把这一近似的幅度量化出来。

[[vk::binding(0, 0)]] RWTexture3D<uint>         u_Global;       // 全局距离（u32 位模式）
[[vk::binding(1, 0)]] RWTexture3D<float>        u_GlobalOut;    // 转换输出（R32F）
[[vk::binding(2, 0)]] Texture3D<float>          u_MeshField;    // 当前注入的 mesh 距离场
[[vk::binding(3, 0)]] SamplerState              u_MeshFieldSampler;
[[vk::binding(4, 0)]] RWStructuredBuffer<float> u_ProbeDist;

[[vk::push_constant]] cbuffer GlobalPC {
    float4 originVoxel;     // xyz = 全局场原点（世界/局部空间）, w = 全局体素边长
    uint4  dims;            // xyz = 全局分辨率, w = mode
    uint4  rangeLo;         // xyz = 注入范围下界（体素）, w = 探针步长
    uint4  rangeHi;         // xyz = 注入范围上界（体素）, w = 未用
    float4 meshOriginVoxel; // xyz = 该 mesh 距离场的原点, w = 该 mesh 的体素边长
    uint4  meshDims;        // xyz = 该 mesh 的分辨率
};

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID) {
    const uint mode = dims.w;

    if (mode == 0u) {                       // ── clear ──
        if (any(id >= dims.xyz)) return;
        u_Global[id] = 0x7F800000u;         // +inf
        return;
    }

    if (mode == 1u) {                       // ── inject ──
        const uint3 v = id;
        if (any(v >= dims.xyz)) return;

        const float3 pWorld = originVoxel.xyz + (float3(v) + 0.5) * originVoxel.w;
        const float3 aabbLo = meshOriginVoxel.xyz;
        const float3 aabbHi = meshOriginVoxel.xyz + float3(meshDims.xyz) * meshOriginVoxel.w;

        // **只在该 mesh 的 AABB 内注入精确的场值**，其余交给跳步洪泛补全（C++ 侧在本 mode 之后跑）。
        // 之前的版本在 AABB 外用"到 AABB 的距离"兜底，实测那是致命的：本场景 mesh 的 AABB 极大
        // （最大边长 3154 单位），min 合并后网格内几乎所有体素都拿到一个极小的值（平均低估
        // 454~569 单位），步进因此在远离表面处就低于阈值 = **提前判命中**，细节层再准也没机会。
        if (any(pWorld < aabbLo) || any(pWorld > aabbHi)) return;

        const float3 uv = ((pWorld - aabbLo) / meshOriginVoxel.w + 0.5) / float3(meshDims.xyz);
        InterlockedMin(u_Global[v], asuint(abs(u_MeshField.SampleLevel(u_MeshFieldSampler, uv, 0))));
        return;
    }

    // ── mode 2: convert + 自检探针 ──
    if (any(id >= dims.xyz)) return;
    const float d = asfloat(u_Global[id]);
    u_GlobalOut[id] = d;

    const uint stride = rangeLo.w;
    if (stride > 0u && (id.x % stride) == 0u && (id.y % stride) == 0u && (id.z % stride) == 0u) {
        const uint nx = dims.x / stride, ny = dims.y / stride, nz = dims.z / stride;
        const uint idx = (id.z / stride) * nx * ny + (id.y / stride) * nx + (id.x / stride);
        if (idx < nx * ny * nz) u_ProbeDist[idx] = d;
    }
}
