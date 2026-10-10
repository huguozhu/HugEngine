#pragma once

#include "Pipeline/Material.h"
#include "Pipeline/Camera.h"
#include "Math/Geometry.h"
#include "RHI/RHI.h"
// 阶段 1 T1.3b-3：收集改由快照承担（`SceneSnapshotBuilder`），本类只做"快照 → GPU 记录"。
#include "Threading/FrameSceneSnapshot.h"
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

// GPU 端场景物体数据（std430 布局）——**定义已收敛到 `ShaderTypes.slang`**（C++/Slang 唯一
// 共享契约头）。`Material.h` 在 `namespace he::render` 内 include 它，故 `GPUSceneObject`
// 在本命名空间直接可见，无需再写一份（原先这里的重复定义 + 128B 断言已随之收敛）。

static_assert(sizeof(GPUSceneObject) == 128, "GPUSceneObject must match shader std430 layout (128 bytes)");

class GPUScene {
public:
    static constexpr u32 kMaxObjects = kMaxGPUObjects;  // 统一到 Material.h

    bool Initialize(rhi::IRHIDevice* device);
    void Shutdown();

    /// 阶段 1 T1.3b-3 / 第③段：从**快照**收集（收集逻辑集中在 `SceneSnapshotBuilder`）。
    /// 【第③段起这是唯一入口】原先还有一个 `Collect(world, sg, camera)` 过渡重载（内部自建快照），
    /// 现已删除：调用方（两条管线）本帧本来就有快照，自建一份既多余、又要求渲染期持有世界。
    void CollectFromSnapshot(const FrameSceneSnapshot& snapshot);

    /// 快照条目 → GPU 剔除记录的转换（与旧 `FillObj` 逐字段对齐）。
    /// 【为什么内联在头文件】单测要能在**不链接 HugEngineRHI** 的前提下验证它与旧 `FillObj`
    /// 逐位一致（`GPUScene.cpp` 会拉进设备接口的实现符号）。
    [[nodiscard]] static GPUSceneObject MakeObjectRecord(const SnapshotDrawItem& item) {
        GPUSceneObject o{};
        o.localToWorld    = item.object.worldMatrix;   // 快照里已是世界矩阵
        o.boundsMin       = item.object.boundsMin;     // 快照里已是**世界空间** AABB（变换在收集侧做过）
        o.boundsMax       = item.object.boundsMax;
        o.meshIndex       = item.meshIndex;            // 由 MeshBatcher 后填（收集阶段恒 0）
        o.materialIndex   = item.materialIndex;
        o.objectID        = item.objectID;
        o.visibilityFlags = item.visibilityFlags;
        o.indexCount      = item.indexCount;           // 同上：由 MeshBatcher 后填
        o.firstIndex      = item.firstIndex;
        o.vertexOffset    = item.vertexOffset;
        return o;
    }

    /// 任务 24：是否把贴花卡片排除在场景物体之外（Deferred 用 DecalPass 投影贴花）。
    /// **必须在首次 Collect 之前设置**：Collect 首次全量收集后走增量分支，
    /// 中途改变口径会让缓存列表与新口径错位。
    void SetExcludeDecals(bool v) { m_ExcludeDecals = v; }
    bool GetExcludeDecals() const { return m_ExcludeDecals; }

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
    bool m_ExcludeDecals = false;   // 任务 24：贴花由 DecalPass 投影，不进场景物体列表
};

} // namespace he::render
