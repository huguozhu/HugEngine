#pragma once

#include "RHI/Types.h"
#include "RHI/Buffer.h"
#include "RHI/Shader.h"
#include "RHI/QueryPool.h"
#include "RHI/RayTracing.h"

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

    // ============================================================
    // Device Generated Commands (DGC) — VK_EXT_device_generated_commands
    // GPU 生成实际 vkCmdDraw* 命令，CPU 仅调用一次 ExecuteGeneratedCommands。
    // 默认空实现（后端不支持时自动跳过）
    // ============================================================

    /// DGC 执行描述符（封装后端特定句柄 + GPU 地址）
    struct DGCExecuteDesc {
        void*   indirectCommandsLayout  = nullptr;  // VkIndirectCommandsLayoutEXT 句柄
        void*   indirectExecutionSet    = nullptr;  // VkIndirectExecutionSetEXT 句柄
        u64     sequencesBufferAddr     = 0;        // 序列数据缓冲 GPU 地址
        u32     maxSequenceCount        = 0;        // 最大序列数
        u64     sequenceCountAddr       = 0;        // 实际序列数缓冲 GPU 地址
        u64     preprocessBufferAddr    = 0;        // 预处理缓冲 GPU 地址
        u64     preprocessBufferSize    = 0;        // 预处理缓冲大小
        u32     maxDrawCount            = 0;        // 最大绘制调用数
    };
    virtual void ExecuteGeneratedCommands(const DGCExecuteDesc& desc) {}

    // Mesh Shader 绘制（替代传统 VS+IA 管线）
    virtual void DrawMeshTasks(u32 groupCountX, u32 groupCountY = 1, u32 groupCountZ = 1) = 0;
    virtual void DrawMeshTasksIndirect(IRHIBuffer* buffer, u64 offset,
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

    // Ray Tracing 命令
    virtual void BuildBLAS(IRHIAccelerationStructure* blas, IRHIBuffer* scratchBuffer,
                           const BLASBuildDesc& desc, bool update = false) = 0;      // 构建/更新 BLAS
    virtual void BuildTLAS(IRHIAccelerationStructure* tlas, IRHIBuffer* scratchBuffer,
                           IRHIBuffer* instanceBuffer, u32 instanceCount,
                           bool update = false) = 0;                                  // 构建/更新 TLAS
    virtual void BindRTPipeline(IRHIRayTracingPipelineState* rtPSO) = 0;              // 绑定 RT 管线
    virtual void TraceRays(const SBTDesc& sbt, u32 width, u32 height, u32 depth = 1) = 0;  // 发射光线

    // 缓冲拷贝（GPU 端）
    virtual void CopyBuffer(IRHIBuffer* src, IRHIBuffer* dst,
                            u64 size, u64 srcOffset = 0, u64 dstOffset = 0) = 0;

    // 纹理拷贝（GPU 端，同分辨率/同格式）
    // 内部自动处理 TRANSFER_SRC / TRANSFER_DST 布局转换
    virtual void CopyTextureToTexture(IRHITexture* src, IRHITexture* dst) = 0;

    // 跨队列所有权转移（AsyncCompute Barrier）
    // 当资源从 Graphics 队列移交给 Compute 队列（或反向）时调用
    // Vulkan: PipelineBarrier 中设置 srcQueueFamilyIndex != dstQueueFamilyIndex
    // D3D12:  发出 ResourceBarrier with D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES
    virtual void QueueOwnershipTransfer(
        IRHITexture* texture,
        QueueType srcQueue,
        QueueType dstQueue,
        ResourceState currentState,
        ResourceState newState) = 0;

    virtual void QueueOwnershipTransfer(
        IRHIBuffer* buffer,
        QueueType srcQueue,
        QueueType dstQueue,
        ResourceState currentState,
        ResourceState newState) = 0;

    // 简化版: 释放资源到目标队列，保持原状态
    virtual void ReleaseToQueue(IRHITexture* texture, QueueType dstQueue) = 0;
    virtual void AcquireFromQueue(IRHITexture* texture, QueueType srcQueue) = 0;

    // 跨队列同步：在 Submit 时携带 Timeline Semaphore Signal/Wait
    // 调用 SetTimelineSignal/SetTimelineWait 后，下一次 Submit 会在
    // vkQueueSubmit 的 pNext 中自动附加 VkTimelineSemaphoreSubmitInfo
    virtual void SetTimelineSignal(RHIFenceHandle fence, u64 value) = 0;
    virtual void SetTimelineWait(RHIFenceHandle fence, u64 value) = 0;

    // GPU 时间戳查询
    virtual void WriteTimestamp(IRHIQueryPool* pool, u32 queryIndex) = 0;
    virtual void ResetQueryPool(IRHIQueryPool* pool) = 0;
    virtual void GetQueryResults(IRHIQueryPool* pool, u32 first, u32 count, u64* data) = 0;

    virtual void Submit() = 0;
};

} // namespace he::rhi
