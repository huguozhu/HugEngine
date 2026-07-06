#pragma once

#include "Pipeline/GBufferRenderer.h"

namespace he::render {

// ============================================================
// GBufferRenderer_GPU — GPU Driven 绘制路径
//
// MeshBatcher::Build + FillGPUScene 在 DeferredPipeline::BuildFrameGraph 中完成。
// 此处仅负责 DrawIndexedIndirect。
// ============================================================
class GBufferRenderer_GPU final : public IGBufferRenderer {
public:
    bool Initialize(GBufferContext& ctx) override;
    void Shutdown() override;
    void Render(rhi::IRHICommandList* cmd, GBufferContext& ctx,
                he::World& world, he::SceneGraph& sg,
                const CameraData& camera) override;
};

} // namespace he::render
