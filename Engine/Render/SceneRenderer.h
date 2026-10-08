#pragma once

#include "Pipeline/Material.h"
#include "Pipeline/Camera.h"
#include "RHI/RHI.h"
#include "Core/Types.h"
#include <vector>

namespace he { class World; class SceneGraph; class MeshComponent; }

namespace he::render {

// ============================================================================
// DrawItem — SceneRenderer::Prepare 输出的单条绘制项
// ============================================================================
struct DrawItem {
    he::MeshComponent* mesh = nullptr;
    u32 objectIndex = 0;  // GPUObjectData SSBO 中的索引
    bool bInstanced = false;  // 实例化网格：普通绘制循环跳过，由实例化 Pass 单次 Draw 渲染
    /// 渲染侧 mesh 注册表索引（阶段 1 附录 E / E-3）：从组件字段透传。
    /// 【为什么要它】绘制路径此前用**组件地址**反查对象条目（`di.mesh == &sm`），那是"渲染侧持有
    /// 组件指针"的一类来源；有了索引后查找/比较都走整数，组件指针可以逐步退出。
    /// 0 = 未注册（此时消费侧用旧行为兜底，不静默错配）。
    u32 meshIndex = 0;
};

// ============================================================================
// SceneRenderer — 通用几何体数据准备器
//
// 只做实体收集 + 视锥剔除 + GPU 数据上传，不录制 Draw 命令。
// 管线自行遍历 DrawItem 列表，使用自己的 PSO/描述符集/RenderPass 录制。
// ============================================================================
class SceneRenderer {
public:
    bool enableFrustumCull = true;  // CPU 视锥剔除开关（默认开启）

    /// 收集可见实体 → 剔除 → 上传 GPUObjectData → 返回 DrawList
    /// @param excludeDecals 跳过贴花卡片（任务 24：Deferred 用 DecalPass 投影贴花；
    ///        卡片若也进 GBuffer 会把地面 albedo 覆盖成"贴花自己的平面"，既重复又错误）
    /// 【为什么要显式传】贴花必须与 MeshBatcher / GPUScene 的收集口径**一致**地排除，
    /// 否则三者枚举出的 objectIndex 会错位（`FillGPUScene` 按顺序对齐）。
    std::vector<DrawItem> Prepare(he::World& world, he::SceneGraph& sg,
                                   const CameraData& camera,
                                   rhi::IRHIBuffer* objectBuffer,
                                   bool excludeDecals = false);
};

} // namespace he::render
