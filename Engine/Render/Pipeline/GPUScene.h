#pragma once

#include "Pipeline/Material.h"
#include "Math/Geometry.h"
#include "RHI/RHI.h"
#include <vector>
#include <memory>

// 前向声明
namespace he { class World; class SceneGraph; }

// ============================================================
// GPUScene — GPU 场景数据管线
//
// 所有可见物体的 Transform + AABB + Mesh 引用 + 材质索引
// 统一存储在一个 SSBO 中，GPU Compute Shader 可直接遍历。
//
// 每帧: Collect() → Upload() → GPU 可直接读取
// Phase 1: 全量上传; Phase 2: Dirty Flag 增量更新
// ============================================================

namespace he::render {

// GPU 端场景物体数据（std140 对齐，256 bytes）
struct alignas(256) GPUSceneObject {
    float4x4 localToWorld;   // [0..64]   世界矩阵
    float4   boundsMin;      // [64..80]  AABB 最小角（世界空间）
    float4   boundsMax;      // [80..96]  AABB 最大角
    u32      meshIndex;      // [96]      全局 Mesh 数组索引
    u32      materialIndex;  // [100]     全局 Material 数组索引
    u32      objectID;       // [104]     GPUObjectData SSBO 中的索引
    u32      visibilityFlags;// [108]     可见性标志（1=visible, 2=castShadow）
    u32      _pad[5];        // [112..128] padding to 128
    // [128..256] reserved for future use (prevLocalToWorld, etc.)
};

static_assert(sizeof(GPUSceneObject) == 256, "GPUSceneObject must be 256 bytes");

class GPUScene {
public:
    static constexpr u32 kMaxObjects = 2048;

    bool Initialize(rhi::IRHIDevice* device);
    void Shutdown();

    /// 从 World 收集所有可渲染物体的数据
    void Collect(class World& world, class SceneGraph& sg);

    /// 上传到 GPU（全量覆盖 SSBO）
    void Upload(rhi::IRHIDevice* device);

    // GPU 缓冲访问
    rhi::IRHIBuffer* GetObjectBuffer() const { return m_ObjectSSBO.get(); }
    u32              GetObjectCount() const { return m_ObjectCount; }

    // CPU 端数据（供 GPU Culling / Indirect Draw 使用）
    const std::vector<GPUSceneObject>& GetObjects() const { return m_Objects; }

private:
    std::vector<GPUSceneObject> m_Objects;
    std::unique_ptr<rhi::IRHIBuffer> m_ObjectSSBO;
    u32 m_ObjectCount = 0;
    bool m_Initialized = false;
};

} // namespace he::render
