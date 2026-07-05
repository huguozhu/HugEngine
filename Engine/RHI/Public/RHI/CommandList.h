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

    // 辅助命令缓冲接口（Phase 2 并行录制）
    virtual void BeginSecondary(IRHIPipelineState* pso) = 0;
    virtual void ExecuteSecondary(IRHICommandList* secondary) = 0;
    virtual bool IsSecondary() const = 0;

    // Render pass: binds framebuffer and clears
    enum class LoadOp : u8 { Clear = 0, Load = 1 };  // Clear=清屏, Load=保留内容
    virtual void BeginRenderPass(
        u32 colorCount,
        Format colorFormat,
        Format depthFormat   = Format::Unknown,
        const ClearValue* clear = nullptr,
        LoadOp loadOp = LoadOp::Clear
    ) = 0;
    virtual void EndRenderPass() = 0;

    // 离屏渲染通道：渲染到自定义纹理附件（非 SwapChain）
    // colorImageView/depthImageView 为后端特定句柄（VkImageView），可空。
    // 必须先调用 SetPipeline 设置匹配的 PSO（render pass 一致）。
    // allowSecondary: true → RP 允许执行 Secondary CB（多线程录制）
    virtual void BeginOffscreenPass(
        void* colorImageView,   // 可空（深度专用通道如 ShadowMap）
        void* depthImageView,   // 可空
        u32 width, u32 height,
        const ClearValue* clear = nullptr,
        bool allowSecondary = false
    ) = 0;
    // MRT 版本：多个颜色附件（用于 Deferred GBuffer / RSM 等）
    virtual void BeginOffscreenPassMRT(
        void* const* colorImageViews, u32 colorCount,
        void* depthImageView,
        u32 width, u32 height,
        const ClearValue* clears = nullptr,
        bool allowSecondary = false
    ) = 0;
    virtual void EndOffscreenPass() = 0;

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
    virtual void DrawIndexedIndirect(IRHIBuffer* buffer, u64 offset,
                                     u32 drawCount, u32 stride) = 0;

    // Push constants（小型常量数据，直接推送到 GPU 寄存器）
    virtual void BindDescriptorSet(u32 setIndex, DescriptorSetHandle set) = 0;

    virtual void SetPushConstants(u32 offset, u32 size, const void* data) = 0;

    // Pipeline barrier（全局内存屏障）
    virtual void PipelineBarrier(
        PipelineStage srcStage, PipelineStage dstStage,
        ResourceState srcState, ResourceState dstState) = 0;

    // Pipeline barrier（图像布局转换，如阴影贴图 Depth→ShaderRead）
    virtual void PipelineBarrier(
        PipelineStage srcStage, PipelineStage dstStage,
        ResourceState srcState, ResourceState dstState,
        IRHITexture* texture) = 0;

    // 计算着色器调度
    virtual void Dispatch(u32 groupCountX, u32 groupCountY, u32 groupCountZ) = 0;
    virtual void DispatchIndirect(IRHIBuffer* buffer, u64 offset) = 0;

    // 缓冲拷贝（GPU 端）
    virtual void CopyBuffer(IRHIBuffer* src, IRHIBuffer* dst,
                            u64 size, u64 srcOffset = 0, u64 dstOffset = 0) = 0;

    virtual void Submit() = 0;
};

} // namespace he::rhi
