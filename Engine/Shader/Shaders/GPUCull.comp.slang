// ============================================================
// GPUCull.comp.slang — GPU 视锥剔除 Compute Shader
//
// 每线程处理一个物体：AABB vs 6 视锥平面测试。
// 平面由 CPU 预计算通过 push constant 传入。
// 可见物体的 objectIndex 写入 u_VisibleIndices。
// ============================================================

struct ObjectBounds {
    float4 minPoint;    // AABB 最小角（世界空间）, w=padding
    float4 maxPoint;    // AABB 最大角, w=padding
    uint   objectIndex; // GPUObjectData 中的索引
    uint   _pad[3];
};

[[vk::binding(0, 0)]] StructuredBuffer<ObjectBounds> u_ObjectBounds;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> u_VisibleIndices;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> u_DrawCount;   // atomic counter at [0]

// Push Constants: 6 个视锥平面 + 物体总数
[[vk::push_constant]] cbuffer CullParams {
    float4 frustumPlanes[6];  // xyz = normal, w = distance（CPU 预计算）
    uint   objectCount;
    uint   _pad[3];
};

// p-vertex 法：AABB 是否在平面背面
bool AABBOutside(float3 aabbMin, float3 aabbMax, float4 plane) {
    float3 n = plane.xyz;
    float3 pVertex;
    pVertex.x = (n.x >= 0.0) ? aabbMax.x : aabbMin.x;
    pVertex.y = (n.y >= 0.0) ? aabbMax.y : aabbMin.y;
    pVertex.z = (n.z >= 0.0) ? aabbMax.z : aabbMin.z;
    return dot(n, pVertex) + plane.w < 0.0;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= objectCount) return;

    ObjectBounds b = u_ObjectBounds[id.x];
    float3 mn = b.minPoint.xyz;
    float3 mx = b.maxPoint.xyz;

    // 测试 6 个平面，任意一个在后面则剔除
    if (AABBOutside(mn, mx, frustumPlanes[0])) return;
    if (AABBOutside(mn, mx, frustumPlanes[1])) return;
    if (AABBOutside(mn, mx, frustumPlanes[2])) return;
    if (AABBOutside(mn, mx, frustumPlanes[3])) return;
    if (AABBOutside(mn, mx, frustumPlanes[4])) return;
    if (AABBOutside(mn, mx, frustumPlanes[5])) return;

    uint idx;
    InterlockedAdd(u_DrawCount[0], 1, idx);
    u_VisibleIndices[idx] = b.objectIndex;
}
