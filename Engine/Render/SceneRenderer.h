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
    std::vector<DrawItem> Prepare(he::World& world, he::SceneGraph& sg,
                                   const CameraData& camera,
                                   rhi::IRHIBuffer* objectBuffer);
};

} // namespace he::render
