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

// Hi-Z 深度金字塔（mip 0 = 全分辨率）
[[vk::binding(3, 0)]] Texture2D<float> u_HiZDepth;
[[vk::binding(3, 0)]] SamplerState     u_HiZSampler;

// Push Constants: 6 个视锥平面 + VP + Hi-Z 参数 + 物体总数
[[vk::push_constant]] cbuffer CullParams {
    float4 frustumPlanes[6];
    float4x4 viewProjMatrix;     // 用于 AABB 投影到屏幕空间
    float2   screenSize;         // 视口尺寸
    uint     hizMipCount;        // Hi-Z mip 层数
    uint     objectCount;
};

bool AABBOutside(float3 aabbMin, float3 aabbMax, float4 plane) {
    float3 n = plane.xyz;
    float3 pVertex;
    pVertex.x = (n.x >= 0.0) ? aabbMax.x : aabbMin.x;
    pVertex.y = (n.y >= 0.0) ? aabbMax.y : aabbMin.y;
    pVertex.z = (n.z >= 0.0) ? aabbMax.z : aabbMin.z;
    return dot(n, pVertex) + plane.w < 0.0;
}

// Hi-Z 遮挡测试：投影 AABB 到屏幕 → 采样深度金字塔 → 比较深度
bool IsOccluded(float3 aabbMin, float3 aabbMax) {
    // 8 个角点投影到屏幕
    float2 rectMin = float2(1e10, 1e10);
    float2 rectMax = float2(-1e10, -1e10);
    float  maxZ = 0.0;

    for (int c = 0; c < 8; c++) {
        float3 corner = float3(
            (c & 1) ? aabbMax.x : aabbMin.x,
            (c & 2) ? aabbMax.y : aabbMin.y,
            (c & 4) ? aabbMax.z : aabbMin.z);
        float4 clip = mul(viewProjMatrix, float4(corner, 1.0));
        float3 ndc = clip.xyz / max(clip.w, 0.0001);
        if (clip.w < 0.0) return false;  // 在相机后方 → 可见

        float2 screen = ndc.xy * 0.5 + 0.5;
        rectMin = min(rectMin, screen);
        rectMax = max(rectMax, screen);
        maxZ = max(maxZ, ndc.z);  // Vulkan NDC z ∈ [0,1]
    }

    // 裁剪到屏幕范围
    rectMin = clamp(rectMin, 0.0, 1.0);
    rectMax = clamp(rectMax, 0.0, 1.0);

    // 选择 mip level：基于占据像素数
    float2 size = (rectMax - rectMin) * screenSize;
    float mipLevel = ceil(log2(max(1.0, max(size.x, size.y))));
    mipLevel = clamp(mipLevel, 0.0, float(hizMipCount - 1));

    // 采样深度金字塔（4 个角取 min）
    float4 depths;
    depths.x = u_HiZDepth.SampleLevel(u_HiZSampler, rectMin, mipLevel);
    depths.y = u_HiZDepth.SampleLevel(u_HiZSampler, float2(rectMax.x, rectMin.y), mipLevel);
    depths.z = u_HiZDepth.SampleLevel(u_HiZSampler, float2(rectMin.x, rectMax.y), mipLevel);
    depths.w = u_HiZDepth.SampleLevel(u_HiZSampler, rectMax, mipLevel);
    float minDepth = min(min(depths.x, depths.y), min(depths.z, depths.w));

    // 物体最远点深度 > Hi-Z 最小深度 → 被遮挡
    return maxZ > minDepth;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= objectCount) return;

    GPUSceneObject obj = u_SceneObjects[id.x];
    float3 mn = obj.boundsMin.xyz;
    float3 mx = obj.boundsMax.xyz;

    // Frustum culling
    if (AABBOutside(mn, mx, frustumPlanes[0])) return;
    if (AABBOutside(mn, mx, frustumPlanes[1])) return;
    if (AABBOutside(mn, mx, frustumPlanes[2])) return;
    if (AABBOutside(mn, mx, frustumPlanes[3])) return;
    if (AABBOutside(mn, mx, frustumPlanes[4])) return;
    if (AABBOutside(mn, mx, frustumPlanes[5])) return;

    // Hi-Z occlusion culling（如果金字塔可用）
    if (hizMipCount > 0 && IsOccluded(mn, mx)) return;

    uint idx;
    InterlockedAdd(u_DrawCount[0], 1, idx);
    u_VisibleIndices[idx] = obj.objectID;
}
