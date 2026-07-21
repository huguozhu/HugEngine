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

// GPU 端场景物体数据（std430 布局，与 GPUCull.comp.slang 保持一致）
struct GPUSceneObject {
    float4x4 localToWorld;   // [0..64]
    float4   boundsMin;      // [64..80]
    float4   boundsMax;      // [80..96]
    u32      meshIndex;      // [96]
    u32      materialIndex;  // [100]
    u32      objectID;       // [104]
    u32      visibilityFlags;// [108]
    u32      indexCount;     // [112] IndirectDraw 参数
    u32      firstIndex;     // [116] IndirectDraw 参数
    i32      vertexOffset;   // [120] IndirectDraw 参数
    u32      _pad[1];        // [124..128]
};

static_assert(sizeof(GPUSceneObject) == 128, "GPUSceneObject must match shader std430 layout (128 bytes)");

class GPUScene {
public:
    static constexpr u32 kMaxObjects = kMaxGPUObjects;  // 统一到 Material.h

    bool Initialize(rhi::IRHIDevice* device);
    void Shutdown();

    /// 从 World 收集所有可渲染物体的数据
    void Collect(class World& world, class SceneGraph& sg);

    /// 上传到 GPU（仅 Dirty 部分增量写入 SSBO）
    void Upload(rhi::IRHIDevice* device);

    // GPU 缓冲访问
    rhi::IRHIBuffer* GetObjectBuffer() const { return m_ObjectSSBO.get(); }
    u32              GetObjectCount() const { return m_ObjectCount; }

    const std::vector<GPUSceneObject>& GetObjects() const { return m_Objects; }

private:
    std::vector<GPUSceneObject> m_Objects;
    std::vector<float4x4> m_CachedMatrices;  // 上帧的 localToWorld（检测变化）
    std::vector<u32>      m_DirtyIndices;    // 需要上传的对象索引
    std::unique_ptr<rhi::IRHIBuffer> m_ObjectSSBO;
    u32 m_ObjectCount = 0;
    bool m_Initialized = false;
};

} // namespace he::render
