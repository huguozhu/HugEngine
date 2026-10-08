#pragma once

#include "Pipeline/Material.h"
#include "Pipeline/Camera.h"
#include "RHI/RHI.h"
#include "Core/Types.h"
// 阶段 1 §15.1 第②段：`Prepare` 改为消费快照（`FrameSceneSnapshot`），不再读世界
#include "Threading/FrameSceneSnapshot.h"
#include <vector>

namespace he { class World; class SceneGraph; class MeshComponent; }

namespace he::render {

// ============================================================================
// DrawItem — SceneRenderer::Prepare 输出的单条绘制项
//
// 【阶段 1 §15.1 第②段：不再持有组件指针】`mesh` 字段已删除 —— 顶点/索引缓冲与索引数一律
// 按 `meshIndex` 去 `MeshRegistry` 查（渲染侧自己的表）。这是附录 E"组件指针闸门"
// （`--mesh-ptrs`）的收敛点：渲染侧不再持有任何 `*Component*`。
// ============================================================================
struct DrawItem {
    u32 objectIndex = 0;  // GPUObjectData SSBO 中的索引
    bool bInstanced = false;  // 实例化/骨骼网格：普通绘制循环跳过，由专用 Pass 渲染
    /// 渲染侧 mesh 注册表索引（阶段 1 附录 E / E-3）：快照条目透传。
    /// 0 = 未注册（消费侧 `Find(0)` 返回空并跳过 —— 可见化，而不是指错资源）。
    u32 meshIndex = 0;
};

// ============================================================================
// SceneRenderer — 通用几何体数据准备器
//
// 只做**视锥剔除 + GPU 数据上传**，不录制 Draw 命令、也不再遍历世界。
// 【阶段 1 §15.1 第②段】收集（遍历 ECS、算材质、算世界矩阵/AABB）已经在游戏线程由
// `SceneSnapshotBuilder` 做完并放进快照；本类只消费快照里的值 ⇒ 签名里的 `World&`/
// `SceneGraph&` 一并消失（附录 B1 因此下降），渲染期与 ECS 彻底解耦。
// 管线自行遍历 DrawItem 列表，使用自己的 PSO/描述符集/RenderPass 录制。
// ============================================================================
class SceneRenderer {
public:
    bool enableFrustumCull = true;  // CPU 视锥剔除开关（默认开启）

    /// 从快照取本帧可见物体 → 剔除 → 上传 GPUObjectData → 返回 DrawList
    /// 【为什么不再需要 excludeDecals】贴花是否进 `draws` 在**收集侧**就由
    /// `SceneSnapshotObjectOptions::excludeDecals` 决定了（快照即口径），消费侧无需再传。
    /// 【为什么不再需要 world/sg】条目的世界矩阵、世界 AABB、材质数据都已在快照里算好。
    /// @param objectBuffer GPUObjectData SSBO（按可见顺序写入，下标 = `DrawItem::objectIndex`）
    std::vector<DrawItem> Prepare(const FrameSceneSnapshot& snapshot,
                                  const CameraData& camera,
                                  rhi::IRHIBuffer* objectBuffer);
};

} // namespace he::render
