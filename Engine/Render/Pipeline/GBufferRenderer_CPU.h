#pragma once

#include "Pipeline/GBufferRenderer.h"
#include "Pipeline/ParallelRecordHelper.h"   // 录制池改为成员（修退出崩溃，见下方注释）

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
                const FrameSceneSnapshot& snapshot,
                const CameraData& camera) override;

private:
    /// GBuffer 次级列表录制池。
    /// 【为什么是成员而不是函数内 static】原来写成 `static ParallelRecordHelper`：它要到**程序退出**
    /// 才析构，那时 Vulkan 设备/VMA 分配器都已销毁 ⇒ `VulkanCommandList::~VulkanCommandList` 在死设备上
    /// `vkResetEvent` 崩溃（退出期必现，与渲染逻辑无关）。改成成员后生命周期跟着渲染器，
    /// `Shutdown` 里显式释放，顺序就正确了。
    ParallelRecordHelper m_ParallelRecorder;
};

} // namespace he::render
