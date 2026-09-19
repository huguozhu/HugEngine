// ScreenProbe_Gather.comp.slang — 探针布置与自适应合并（步骤 20）
//
// 【目标】每 16×16 像素一个"单元"；相邻 2×2 单元的法线足够一致（平坦区）就合并成**一个探针**
// （占 32×32 像素），否则保留 4 个探针。1080p 下单元数 120×68 = 8160，设计量级 ≈ 8K 探针；
// 合并后数量应显著下降，且**随合并阈值单调变化**。
//
// 【为什么同时写"偏差缓冲"】合并阈值是超参，要证明"阈值变化 ⇒ 探针数单调"最好是**一次运行**
// 就把整条曲线算出来。所以本 pass 除了按当前阈值写探针，还把每个 32×32 tile 内 4 个单元的
// **最大法线偏差**写进 `u_TileDev`；CPU 侧对同一个缓冲取不同阈值即可得到完整曲线（不必反复重跑）。
#include "Lumen/ScreenProbe.slang"

[[vk::binding(0, 0)]] Texture2D<float4> u_GBufferNormal;   // xyz = n*0.5+0.5
[[vk::binding(1, 0)]] Texture2D<float4> u_GBufferWorldPos; // xyz = 世界坐标（alpha 是 dielectricF0）
[[vk::binding(2, 0)]] RWStructuredBuffer<ScreenProbe> u_Probes;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint>        u_ProbeCount;   // [0] = 已写探针数
[[vk::binding(4, 0)]] RWStructuredBuffer<float>       u_TileDev;      // 每个 32×32 tile 的最大法线偏差（cos 值）
// 步骤 24：每个 16×16 单元的"归属探针"下标（0xFFFFFFFF = 无）。合成端要按像素查探针，
// 若在片元里遍历上万探针找最近，代价无法接受；这里顺手把映射写出来，查询变成一次数组读。
[[vk::binding(5, 0)]] RWStructuredBuffer<uint>        u_CellProbe;

[[vk::push_constant]] cbuffer ProbePC {
    uint4  dims;        // xy = 屏幕尺寸, z = 探针容量, w = **单元**数（cellsX*cellsY）
    float4 mergeParams; // x = 法线一致阈值（cos，越小越宽松）, y = 单元边长, z/w 保留
};

float3 DecodeNormal(float4 n) { return normalize(n.xyz * 2.0 - 1.0); }

// 一个 16×16 单元的平均法线与平均世界坐标（步长 4 采样，够稳且便宜）
void SampleCell(uint2 cellOrigin, out float3 meanN, out float3 meanP, out float valid) {
    float3 sn = float3(0.0), sp = float3(0.0);
    float n = 0.0;
    for (uint y = 4u; y < 16u; y += 8u) {
        for (uint x = 4u; x < 16u; x += 8u) {
            const int2 px = int2(cellOrigin + uint2(x, y));
            const float4 wp = u_GBufferWorldPos.Load(int3(px, 0));
            if (all(abs(wp.xyz) < 1e-4)) continue;
            sp += wp.xyz;
            sn += DecodeNormal(u_GBufferNormal.Load(int3(px, 0)));
            n += 1.0;
        }
    }
    valid = (n > 0.0) ? 1.0 : 0.0;
    meanN = (n > 0.0) ? normalize(sn / n) : float3(0.0, 0.0, 1.0);
    meanP = (n > 0.0) ? (sp / n) : float3(0.0);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    const uint tilesX = (dims.x + 31u) / 32u;   // 以 32×32 为合并单位
    const uint tilesY = (dims.y + 31u) / 32u;
    if (tid.x >= tilesX || tid.y >= tilesY) return;

    const uint2 tileOrigin = uint2(tid.x * 32u, tid.y * 32u);
    const uint  tileIndex  = tid.y * tilesX + tid.x;
    const uint  cellsX     = (dims.x + 15u) / 16u;
    // 4 个单元在"单元网格"里的线性下标
    const uint cellIndex[4] = {
        (tid.y * 2u)     * cellsX + tid.x * 2u,
        (tid.y * 2u)     * cellsX + tid.x * 2u + 1u,
        (tid.y * 2u + 1u) * cellsX + tid.x * 2u,
        (tid.y * 2u + 1u) * cellsX + tid.x * 2u + 1u,
    };

    // 4 个 16×16 单元
    float3 n[4];
    float3 p[4];
    float  v[4];        // 该单元是否有效（float 便于直接参与计数）
    uint2 origins[4] = { tileOrigin, tileOrigin + uint2(16, 0), tileOrigin + uint2(0, 16), tileOrigin + uint2(16, 16) };
    float3 meanN = float3(0.0);
    float  validCells = 0.0;
    for (uint i = 0; i < 4u; ++i) {
        float valid;
        SampleCell(origins[i], n[i], p[i], valid);
        v[i] = valid;
        if (valid > 0.5) { meanN += n[i]; validCells += 1.0; }
    }
    // 先整体标成"无探针"；下面每写一个探针就把它覆盖的单元填上
    for (uint i = 0; i < 4u; ++i) {
        if (cellIndex[i] < dims.w) u_CellProbe[cellIndex[i]] = 0xFFFFFFFFu;
    }
    if (validCells < 0.5) { u_TileDev[tileIndex] = -1.0; return; }   // 整块没几何
    meanN = normalize(meanN);

    // 最大法线偏差（用 cos：越接近 1 越平坦）
    float worstCos = 1.0;
    for (uint i = 0; i < 4u; ++i)
        if (v[i] > 0.5) worstCos = min(worstCos, dot(meanN, n[i]));
    u_TileDev[tileIndex] = worstCos;

    // 合并判定：4 个单元都有几何 且 最差偏差在阈值内 ⇒ 1 个探针（取 4 单元均值），否则每个有效单元 1 个
    const bool merge = (validCells > 3.5) && (worstCos >= mergeParams.x);
    if (merge) {
        uint slot;
        InterlockedAdd(u_ProbeCount[0], 1u, slot);
        if (slot < dims.z) {
            float3 mp = (p[0] + p[1] + p[2] + p[3]) * 0.25;
            ScreenProbe probe;
            probe.position = float4(mp, 1.0);
            probe.normal   = float4(meanN, 0.0);
            probe.uv       = float4((float2(tileOrigin) + 16.0) / float2(dims.xy), (float2)tid.xy);
            probe.shR = float4(0.0); probe.shG = float4(0.0); probe.shB = float4(0.0);
            probe.meta = uint4(4u, tileIndex, (uint)(worstCos * 65535.0), 0u);
            u_Probes[slot] = probe;
            // 合并探针覆盖它的 4 个单元
            for (uint i = 0; i < 4u; ++i)
                if (cellIndex[i] < dims.w) u_CellProbe[cellIndex[i]] = slot;
        }
    } else {
        for (uint i = 0; i < 4u; ++i) {
            if (v[i] < 0.5) continue;
            uint slot;
            InterlockedAdd(u_ProbeCount[0], 1u, slot);
            if (slot < dims.z) {
                ScreenProbe probe;
                probe.position = float4(p[i], 1.0);
                probe.normal   = float4(n[i], 0.0);
                probe.uv       = float4((float2(origins[i]) + 8.0) / float2(dims.xy), (float2)tid.xy);
                probe.shR = float4(0.0); probe.shG = float4(0.0); probe.shB = float4(0.0);
                probe.meta = uint4(1u, tileIndex, (uint)(worstCos * 65535.0), i);
                u_Probes[slot] = probe;
                if (cellIndex[i] < dims.w) u_CellProbe[cellIndex[i]] = slot;
            }
        }
    }
}
