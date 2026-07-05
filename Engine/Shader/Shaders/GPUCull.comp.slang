// GPUCull.comp.slang — GPU 视锥剔除（读取 GPUScene SSBO）
// 每线程测试一个物体的 AABB vs 6 视锥平面

// 匹配 C++ GPUSceneObject（256 bytes, std140）
struct GPUSceneObject {
    float4x4 localToWorld;   // [0..64]
    float4   boundsMin;      // [64]
    float4   boundsMax;      // [80]
    uint     meshIndex;      // [96]
    uint     materialIndex;  // [100]
    uint     objectID;       // [104]
    uint     visibilityFlags;// [108]
    uint     _pad[5];        // [112..128]
    // [128..256] reserved
};

// 输入：所有物体的 AABB（从 GPUScene SSBO 读取）
[[vk::binding(0, 0)]] StructuredBuffer<GPUSceneObject> u_SceneObjects;

// 输出：可见物体的 objectIndex 列表
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> u_VisibleIndices;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> u_DrawCount;

// Push Constants: 6 个视锥平面 + 物体总数
[[vk::push_constant]] cbuffer CullParams {
    float4 frustumPlanes[6];
    uint   objectCount;
    uint   _pad[3];
};

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

    GPUSceneObject obj = u_SceneObjects[id.x];
    float3 mn = obj.boundsMin.xyz;
    float3 mx = obj.boundsMax.xyz;

    if (AABBOutside(mn, mx, frustumPlanes[0])) return;
    if (AABBOutside(mn, mx, frustumPlanes[1])) return;
    if (AABBOutside(mn, mx, frustumPlanes[2])) return;
    if (AABBOutside(mn, mx, frustumPlanes[3])) return;
    if (AABBOutside(mn, mx, frustumPlanes[4])) return;
    if (AABBOutside(mn, mx, frustumPlanes[5])) return;

    uint idx;
    InterlockedAdd(u_DrawCount[0], 1, idx);
    u_VisibleIndices[idx] = obj.objectID;
}
