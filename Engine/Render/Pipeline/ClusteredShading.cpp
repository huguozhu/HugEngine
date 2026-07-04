#include "Pipeline/ClusteredShading.h"
#include "Core/Log.h"
#include <algorithm>
#include <cmath>

// ============================================================
// ClusteredShading 实现
// ============================================================

namespace he::render {

// Z 切片深度：对数分布，近处密度更高
float ClusteredShading::SliceDepth(u32 slice, float nearZ, float farZ) const {
    float t = static_cast<float>(slice) / static_cast<float>(kDepthSlices);
    // 对数分布: z = near * (far/near)^t
    return nearZ * std::pow(farZ / nearZ, t);
}

void ClusteredShading::BuildClusters(const float4x4& invProj, u32 screenW, u32 screenH,
                                      float nearZ, float farZ) {
    m_TilesX = (screenW + kTileSize - 1) / kTileSize;
    m_TilesY = (screenH + kTileSize - 1) / kTileSize;
    m_ClusterCount = m_TilesX * m_TilesY * kDepthSlices;

    m_Clusters.resize(m_ClusterCount);

    for (u32 z = 0; z < kDepthSlices; ++z) {
        float depthNear = SliceDepth(z, nearZ, farZ);
        float depthFar  = SliceDepth(z + 1, nearZ, farZ);

        for (u32 ty = 0; ty < m_TilesY; ++ty) {
            for (u32 tx = 0; tx < m_TilesX; ++tx) {
                u32 idx = ClusterIndex(tx, ty, z);

                // 屏幕空间 tile 范围 → NDC
                float left   = (static_cast<float>(tx * kTileSize) / screenW) * 2.0f - 1.0f;
                float right  = (static_cast<float>(std::min((tx + 1) * kTileSize, screenW)) / screenW) * 2.0f - 1.0f;
                float top    = 1.0f - (static_cast<float>(ty * kTileSize) / screenH) * 2.0f;  // Vulkan Y 翻转
                float bottom = 1.0f - (static_cast<float>(std::min((ty + 1) * kTileSize, screenH)) / screenH) * 2.0f;

                // Vulkan NDC: z ∈ [0,1], 0=near, 1=far
                // 但逆投影变换用 clip space: z ∈ [0, w]
                float zNearNDC = depthNear / farZ;  // 近似映射到 [0,1]
                float zFarNDC  = depthFar  / farZ;

                // 8 个角点: NDC (x,y,z) → clip → world (除以 w)
                float3 corners[8] = {
                    {left,  bottom, zNearNDC}, {right, bottom, zNearNDC},
                    {right, top,    zNearNDC}, {left,  top,    zNearNDC},
                    {left,  bottom, zFarNDC},  {right, bottom, zFarNDC},
                    {right, top,    zFarNDC},  {left,  top,    zFarNDC},
                };

                float3 aabbMin(std::numeric_limits<float>::max());
                float3 aabbMax(std::numeric_limits<float>::lowest());

                for (int c = 0; c < 8; ++c) {
                    float4 clipPos(corners[c].x, corners[c].y, corners[c].z, 1.0f);
                    float4 worldPos = invProj * clipPos;
                    float3 wp = float3(worldPos) / worldPos.w;
                    aabbMin = glm::min(aabbMin, wp);
                    aabbMax = glm::max(aabbMax, wp);
                }

                m_Clusters[idx].minPoint = float4(aabbMin, 0.0f);
                m_Clusters[idx].maxPoint = float4(aabbMax, 0.0f);
            }
        }
    }

    static bool s_FirstBuild = true;
    if (s_FirstBuild) {
        HE_CORE_INFO("ClusteredShading: {}×{}×{} = {} clusters (tile={}px)",
            m_TilesX, m_TilesY, kDepthSlices, m_ClusterCount, kTileSize);
        s_FirstBuild = false;
    }
}

void ClusteredShading::CullLights(const GPULight* lights, u32 lightCount) {
    m_LightGrid.assign(m_ClusterCount, {0, 0});
    m_LightIndexList.clear();
    m_LightIndexList.reserve(m_ClusterCount * kMaxLightsPerCluster);

    for (u32 ci = 0; ci < m_ClusterCount; ++ci) {
        u32 offset = static_cast<u32>(m_LightIndexList.size());
        u32 count  = 0;

        float3 cMin = float3(m_Clusters[ci].minPoint);
        float3 cMax = float3(m_Clusters[ci].maxPoint);

        for (u32 li = 0; li < lightCount; ++li) {
            const auto& light = lights[li];
            bool visible = false;

            u32 ltype = static_cast<u32>(light.directionType.w);
            if (ltype == 0u) {
                // 方向光影响所有 cluster
                visible = true;
            } else {
                // 点光源/聚光灯：球体-AABB 相交测试
                float3 lightPos = float3(light.positionRange);
                float  range    = light.positionRange.w;

                // 找到 AABB 上最近点
                float3 closest = glm::clamp(lightPos, cMin, cMax);
                float dist2 = glm::dot(lightPos - closest, lightPos - closest);
                visible = (dist2 <= range * range);
            }

            if (visible && count < kMaxLightsPerCluster) {
                m_LightIndexList.push_back(li);
                ++count;
            }
        }

        m_LightGrid[ci] = {offset, count};
    }

    m_TotalLightRefs = static_cast<u32>(m_LightIndexList.size());
    // 日志仅在首帧或集群数变化时输出
    static u32 s_LastRefs = 0xFFFFFFFF;
    if (m_TotalLightRefs != s_LastRefs) {
        HE_CORE_INFO("ClusteredShading culled: {} light refs / {} clusters ({} lights)",
            m_TotalLightRefs, m_ClusterCount, lightCount);
        s_LastRefs = m_TotalLightRefs;
    }
}

} // namespace he::render
