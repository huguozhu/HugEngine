#pragma once

#include "RHI/Types.h"
#include "RHI/Buffer.h"
#include "RHI/Shader.h"

namespace he::rhi {

class IRHISwapChain;  // 前向声明，避免循环依赖

struct ClearValue {
    float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float depth    = 1.0f;
    u8    stencil  = 0;
};

struct Viewport {
    float x = 0, y = 0;
    float width  = 1920;
    float height = 1080;
    float minDepth = 0.0f;
    float maxDepth = 1.0f;
};

struct ScissorRect {
    i32 x = 0, y = 0;
    u32 width = 1920, height = 1080;
};

class IRHICommandList {
public:
    virtual ~IRHICommandList() = default;

    virtual void Begin() = 0;
    virtual void End()   = 0;

    // Render pass: binds framebuffer and clears
    virtual void BeginRenderPass(
        u32 colorCount,
        Format colorFormat,
        Format depthFormat   = Format::Unknown,
        const ClearValue* clear = nullptr
    ) = 0;
    virtual void EndRenderPass() = 0;

    // SwapChain 关联（自动管理 Framebuffer + 同步 + 图像索引）
    virtual void SetSwapChain(IRHISwapChain* swapchain) = 0;

    // Pipeline + resources
    virtual void SetPipeline(IRHIPipelineState* pso) = 0;
    virtual void SetVertexBuffer(IRHIBuffer* buffer, u32 binding = 0) = 0;
    virtual void SetIndexBuffer(IRHIBuffer* buffer, u32 offset = 0) = 0;

    // State
    virtual void SetViewport(const Viewport& vp) = 0;
    virtual void SetScissor(const ScissorRect& sc) = 0;

    // Draw
    virtual void Draw(u32 vertexCount, u32 instanceCount = 1,
                      u32 firstVertex = 0, u32 firstInstance = 0) = 0;
    virtual void DrawIndexed(u32 indexCount, u32 instanceCount = 1,
                             u32 firstIndex = 0, i32 vertexOffset = 0,
                             u32 firstInstance = 0) = 0;

    // Push constants（小型常量数据，直接推送到 GPU 寄存器）
    virtual void SetPushConstants(u32 offset, u32 size, const void* data) = 0;

    // Pipeline barrier（资源状态转换 / 同步）
    virtual void PipelineBarrier(
        PipelineStage srcStage, PipelineStage dstStage,
        ResourceState srcState, ResourceState dstState) = 0;

    // 缓冲拷贝（GPU 端）
    virtual void CopyBuffer(IRHIBuffer* src, IRHIBuffer* dst,
                            u64 size, u64 srcOffset = 0, u64 dstOffset = 0) = 0;

    virtual void Submit() = 0;
};

} // namespace he::rhi
