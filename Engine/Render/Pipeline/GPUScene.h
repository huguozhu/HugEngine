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

    /// 【T7】GPU 驱动 GBuffer：按**合批顺序**重建对象表（下标 = 合批序号）。
    ///
    /// 【为什么需要另建入口】GPU 剔除写出的间接命令用 `objectID` 填 `firstInstance`，而绘制端
    ///   用它当物体缓冲下标（`GBuffer.vert.slang` 的 `useInstanceID == 1` 分支）。因此三处必须
    ///   是同一个序号：GPUScene 对象下标、间接命令 `firstInstance`、物体缓冲下标 —— 也就是
    ///   **合批序号**。原实现按快照全量顺序填表，而物体缓冲却是 CPU 视锥裁剪后的可见子集，
    ///   两者顺序/长度都不同，直接放开就会画错物体（T7 记录的三处问题之一）。
    ///
    /// 【实例化网格】`bInstanced` 条目标记为不可见（`visibilityFlags = 0`）：它们由
    ///   `InstanceCuller` 的逐实例路径单独绘制，不能进 GPU 驱动的间接绘制（否则只会按对象
    ///   矩阵画 1 个实例，而不是 N 个实例）。
    ///
    /// @param snapshot             本帧快照
    /// @param batchSnapshotIndices 合批命令 → 快照条目下标（`MeshBatcher::GetBatchSnapshotIndices`）
    void CollectBatchedFromSnapshot(const FrameSceneSnapshot& snapshot,
                                    const std::vector<u32>& batchSnapshotIndices);

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
    /// A-3：句柄按需解析（代次过期 ⇒ nullptr）
    rhi::IRHIBuffer* GetObjectBuffer() const {
        return m_Device ? m_Device->ResolveBuffer(m_ObjectSSBO) : nullptr;
    }
    u32              GetObjectCount() const { return m_ObjectCount; }

    const std::vector<GPUSceneObject>& GetObjects() const { return m_Objects; }

private:
    std::vector<GPUSceneObject> m_Objects;
    std::vector<float4x4> m_CachedMatrices;  // 上帧的 localToWorld（检测变化）
    std::vector<u32>      m_DirtyIndices;    // 需要上传的对象索引
    rhi::RHIBufferHandle m_ObjectSSBO{};        // A-3：句柄持有
    rhi::IRHIDevice*    m_Device = nullptr;     // A-3：仅借用（Initialize 设置、Shutdown 清空）
    u32 m_ObjectCount = 0;
    bool m_Initialized = false;
    bool m_ExcludeDecals = false;   // 任务 24：贴花由 DecalPass 投影，不进场景物体列表
};

} // namespace he::render
