#pragma once

// ============================================================
// VulkanCommandList.h — Vulkan 命令列表封装
// 从 VulkanInternal.h 拆分
// 依赖 VulkanSwapChain（m_pSwapChain 成员），forward declare VulkanDevice
// ============================================================

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "RHI/CommandList.h"
#include "VulkanSwapChain.h"

#include <vector>
#include <span>

namespace he::rhi {

// 前向声明（避免循环依赖）
class VulkanDevice;

// ============================================================
// VulkanCommandList — 完整定义
// ============================================================
class VulkanCommandList final : public IRHICommandList {
public:
    VulkanCommandList(VkDevice device, VkQueue queue, u32 queueFamily,
                      VulkanDevice* vulkanDevice = nullptr);
    VulkanCommandList(VkDevice device, u32 queueFamily, VulkanDevice* vulkanDevice); // 辅助 CB
    ~VulkanCommandList() override;

    void Begin() override;
    void End()   override;
    void BeginSecondary(IRHIPipelineState* pso) override;
    void ExecuteSecondary(IRHICommandList* secondary) override;
    bool IsSecondary() const override { return m_SecondaryPool != VK_NULL_HANDLE; }
    void BeginRenderPass(u32 colorCount, Format colorFmt, Format depthFmt,
                         const ClearValue* clear, LoadOp loadOp) override;
    void EndRenderPass() override;
    void BeginOffscreenPass(void* colorImageView, void* depthImageView,
                            u32 width, u32 height,
                            const ClearValue* clear, bool allowSecondary) override;
    void BeginOffscreenPassMRT(void* const* colorImageViews, u32 colorCount,
                               void* depthImageView, u32 width, u32 height,
                               const ClearValue* clears, bool allowSecondary) override;
    void EndOffscreenPass() override;
    void SetSwapChain(IRHISwapChain* swapchain) override;
    void SetPipeline(IRHIPipelineState* pso) override;
    void SetVertexBuffer(IRHIBuffer* buffer, u32 binding) override;
    void SetIndexBuffer(IRHIBuffer* buffer, u32 offset) override;
    void SetViewport(const Viewport& vp) override;
    void SetScissor(const ScissorRect& sc) override;
    void BindDescriptorSet(u32 setIndex, DescriptorSetHandle set) override;
    void Draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) override;
    void DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex,
                     i32 vertexOffset, u32 firstInstance) override;
    void DrawIndexedIndirect(IRHIBuffer* buffer, u64 offset,
                             u32 drawCount, u32 stride) override;
    void ExecuteGeneratedCommands(const DGCExecuteDesc& desc) override;
    void DrawMeshTasks(u32 groupCountX, u32 groupCountY, u32 groupCountZ) override;
    void DrawMeshTasksIndirect(IRHIBuffer* buffer, u64 offset,
                               u32 drawCount, u32 stride) override;
    void SetPushConstants(u32 offset, u32 size, const void* data) override;
    void Dispatch(u32 groupCountX, u32 groupCountY, u32 groupCountZ) override;
    void DispatchIndirect(IRHIBuffer* buffer, u64 offset) override;
    void BuildBLAS(IRHIAccelerationStructure* blas, IRHIBuffer* scratchBuffer,
                   const BLASBuildDesc& desc, bool update) override;
    void BuildTLAS(IRHIAccelerationStructure* tlas, IRHIBuffer* scratchBuffer,
                   IRHIBuffer* instanceBuffer, u32 instanceCount, bool update) override;
    void BindRTPipeline(IRHIRayTracingPipelineState* rtPSO) override;
    void TraceRays(const SBTDesc& sbt, u32 width, u32 height, u32 depth) override;
    void PipelineBarrier(PipelineStage srcStage, PipelineStage dstStage,
                         ResourceState srcState, ResourceState dstState) override;
    void PipelineBarrier(PipelineStage srcStage, PipelineStage dstStage,
                         ResourceState srcState, ResourceState dstState,
                         IRHITexture* texture) override;
    void CopyBuffer(IRHIBuffer* src, IRHIBuffer* dst,
                    u64 size, u64 srcOffset, u64 dstOffset) override;
    void CopyTextureToTexture(IRHITexture* src, IRHITexture* dst) override;

    // 跨队列所有权转移
    void QueueOwnershipTransfer(IRHITexture* texture,
                                QueueType srcQueue, QueueType dstQueue,
                                ResourceState currentState, ResourceState newState) override;
    void QueueOwnershipTransfer(IRHIBuffer* buffer,
                                QueueType srcQueue, QueueType dstQueue,
                                ResourceState currentState, ResourceState newState) override;
    void ReleaseToQueue(IRHITexture* texture, QueueType dstQueue) override;
    void AcquireFromQueue(IRHITexture* texture, QueueType srcQueue) override;

    // Timeline Semaphore 集成
    void SetTimelineSignal(RHIFenceHandle fence, u64 value) override;
    void SetTimelineWait(RHIFenceHandle fence, u64 value) override;

    // GPU Timestamp Query
    void WriteTimestamp(IRHIQueryPool* pool, u32 queryIndex) override;
    void ResetQueryPool(IRHIQueryPool* pool) override;
    void GetQueryResults(IRHIQueryPool* pool, u32 first, u32 count, u64* data) override;

    // GPU 通用查询
    void BeginQuery(IRHIQueryPool* pool, u32 queryIndex) override;
    void EndQuery(IRHIQueryPool* pool, u32 queryIndex) override;

    // Debug Label（VK_EXT_debug_utils）
    void BeginDebugLabel(const char* name, const float color[4] = nullptr) override;
    void EndDebugLabel() override;
    void InsertDebugLabel(const char* name, const float color[4] = nullptr) override;

    void Submit() override;

    // Phase 1 桥接
    void SetSwapchainViews(Span<VkImageView> views, VkExtent2D extent) {
        m_SwapchainViews.assign(views.begin(), views.end());
        m_SwapchainExtent = extent;
    }
    void SetCurrentImageIndex(u32 index) { m_CurrentImageIndex = index; }
    void SetQueueType(QueueType type) { m_QueueType = type; }
    QueueType GetQueueType() const { return m_QueueType; }

    void SetSyncSemaphores(VkSemaphore wait, VkSemaphore signal) {
        m_WaitSemaphore   = wait;
        m_SignalSemaphore = signal;
    }

    VkCommandBuffer GetHandle() const { return m_CmdBuffers[m_FrameIndex]; }

private:
    VkDevice         m_Device      = VK_NULL_HANDLE;
    VkQueue          m_Queue       = VK_NULL_HANDLE;
    VulkanDevice*    m_VulkanDevice = nullptr;
    u32              m_QueueFamily = 0;
    QueueType        m_QueueType   = QueueType::Graphics;

    static constexpr u32 kMaxFramesInFlight = 3;
    VkCommandPool    m_CmdPools[kMaxFramesInFlight]   = {};
    VkCommandBuffer  m_CmdBuffers[kMaxFramesInFlight] = {};
    VkFence          m_Fences[kMaxFramesInFlight]     = {};
    u32              m_FrameIndex = 0;

    static constexpr u32 kMaxSecondaryCBs = 3;
    VkCommandPool    m_SecondaryPool = VK_NULL_HANDLE;
    VkCommandBuffer  m_SecCmdBuffers[kMaxSecondaryCBs] = {};
    u32              m_SecSlot   = 0;
    u32              m_SecActive = 0;

    VulkanSwapChain* m_pSwapChain = nullptr;

    VkSemaphore      m_WaitSemaphore   = VK_NULL_HANDLE;
    VkSemaphore      m_SignalSemaphore = VK_NULL_HANDLE;

    VkSemaphore      m_TimelineSignalSem = VK_NULL_HANDLE;
    u64              m_TimelineSignalVal = 0;
    VkSemaphore      m_TimelineWaitSem   = VK_NULL_HANDLE;
    u64              m_TimelineWaitVal   = 0;

    VkPipeline          m_CurrentPipeline = VK_NULL_HANDLE;
    VkPipelineLayout    m_CurrentLayout   = VK_NULL_HANDLE;
    VkRenderPass        m_CurrentRenderPass = VK_NULL_HANDLE;
    VkRenderPass        m_LoadRenderPass = VK_NULL_HANDLE;
    VkRenderPass        m_CurrentFramebufferRP = VK_NULL_HANDLE;
    VkPipelineBindPoint m_CurrentBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    VkBuffer         m_CurrentVB       = VK_NULL_HANDLE;
    u32              m_VBBinding       = 0;
    VkBuffer         m_CurrentIB       = VK_NULL_HANDLE;
    VkIndexType      m_CurrentIndexType = VK_INDEX_TYPE_UINT32;
    u32              m_IBOffset         = 0;

    std::vector<VkImageView> m_SwapchainViews;
    VkExtent2D               m_SwapchainExtent{};
    std::vector<VkFramebuffer> m_Framebuffers;
    bool                       m_FramebuffersNeedRebuild = true;
    u32                       m_CurrentImageIndex = 0;

    bool          m_IsRecording = false;

    VkRenderPass  m_OffscreenRP = VK_NULL_HANDLE;
    bool          m_InOffscreenPass = false;
    VkFramebuffer m_CurrentOffscreenFB = VK_NULL_HANDLE;
    // m_PendingFBs 已移除，改用 m_VulkanDevice->GetDeferredDestroy() 统一管理

    VkImage        m_DummyDepthImage  = VK_NULL_HANDLE;
    VkDeviceMemory m_DummyDepthMemory = VK_NULL_HANDLE;
    VkImageView    m_DummyDepthView   = VK_NULL_HANDLE;
};

} // namespace he::rhi
