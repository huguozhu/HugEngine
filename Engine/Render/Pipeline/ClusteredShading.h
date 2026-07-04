#pragma once

#include "Pipeline/Material.h"
#include "Math/Math.h"
#include "Math/Geometry.h"
#include <vector>

// ============================================================
// ClusteredShading — 视锥空间 Cluster 划分 + 光源剔除
//
// 将视锥体划分为 X×Y×Z 的三维网格，每帧计算每个 cluster
// 的 AABB，测试哪些光源影响它。结果存入 LightGrid + LightIndexList
// 两个 SSBO，供 DeferredLighting 着色器按 cluster 索引查询。
//
// 用法 (每帧):
//   cs.BuildClusters(invProj, screenW, screenH, near, far);
//   cs.CullLights(lights, lightCount);  // CPU 端剔除
//   cs.UploadToGPU(device);             // 上传 LightGrid + LightIndexList
// ============================================================

namespace he::render {

class ClusteredShading {
public:
    // --- 网格参数 ---
    static constexpr u32 kTileSize   = 64;   // 屏幕空间 tile 大小（像素）
    static constexpr u32 kDepthSlices = 12;  // Z 方向切片数（对数分布）
    static constexpr u32 kMaxLightsPerCluster = 64;

    // 每个 cluster 的 AABB（世界空间）
    struct ClusterAABB {
        float4 minPoint;   // xyz=最小角, w=padding
        float4 maxPoint;   // xyz=最大角, w=padding
    };

    // LightGrid: 每个 cluster 的光源列表（offset + count）
    struct LightGridCell {
        u32 offset;   // 在 LightIndexList 中的起始偏移
        u32 count;    // 影响该 cluster 的光源数量
    };

    ClusteredShading() = default;

    // --- 开关 ---
    bool enabled = true;

    // --- 查询 ---
    u32 GetClusterCount()   const { return m_ClusterCount; }
    u32 GetTileCountX()     const { return m_TilesX; }
    u32 GetTileCountY()     const { return m_TilesY; }
    u32 GetTotalLightRefs() const { return m_TotalLightRefs; }

    // --- 每帧调用 ---

    /// 第一步：构建全部 cluster 的 AABB
    void BuildClusters(const float4x4& invProj, u32 screenW, u32 screenH, float nearZ, float farZ);

    /// 第二步：CPU 端光源剔除（将可见光源索引填入 LightGrid）
    void CullLights(const GPULight* lights, u32 lightCount);

    // --- GPU 数据访问（供管线绑定）---
    const std::vector<ClusterAABB>&   GetClusterAABBs()    const { return m_Clusters; }
    const std::vector<LightGridCell>& GetLightGrid()       const { return m_LightGrid; }
    const std::vector<u32>&           GetLightIndexList()  const { return m_LightIndexList; }

private:
    /// 从屏幕坐标 + Z slice index 计算 cluster 索引
    u32 ClusterIndex(u32 tileX, u32 tileY, u32 slice) const {
        return (slice * m_TilesY + tileY) * m_TilesX + tileX;
    }

    /// Z slice 的深度范围（对数分布，近处更密）
    float SliceDepth(u32 slice, float nearZ, float farZ) const;

    u32 m_TilesX = 0, m_TilesY = 0;
    u32 m_ClusterCount = 0;
    u32 m_TotalLightRefs = 0;

    std::vector<ClusterAABB>   m_Clusters;
    std::vector<LightGridCell> m_LightGrid;
    std::vector<u32>           m_LightIndexList;
};

} // namespace he::render
