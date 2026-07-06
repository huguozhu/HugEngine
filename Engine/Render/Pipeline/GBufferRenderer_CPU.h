#pragma once

#include "Pipeline/GBufferRenderer.h"

namespace he::render {

// ============================================================
// GBufferRenderer_CPU — CPU Driven 绘制路径
//
// 逐对象 SetVertexBuffer/SetIndexBuffer/DrawIndexed，
// 使用 push constant objectIndex 模式。
// 逻辑从 DeferredPipeline::BuildFrameGraph 搬迁而来。
// ============================================================
class GBufferRenderer_CPU final : public IGBufferRenderer {
public:
    bool Initialize(GBufferContext& ctx) override;
    void Shutdown() override;
    void Render(rhi::IRHICommandList* cmd, GBufferContext& ctx,
                he::World& world, he::SceneGraph& sg,
                const CameraData& camera) override;
};

} // namespace he::render
