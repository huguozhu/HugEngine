#pragma once

// ============================================================
// VulkanInternal.h — Internal bridge for sample/testing code
//
// NOT part of the public RHI API. Used only by samples
// until the full RHI abstraction is complete (Phase 1-2).
// ============================================================

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

// VMA (Vulkan Memory Allocator) — 单头文件，内存池化与子分配
#define VMA_VULKAN_VERSION 1003000  // Vulkan 1.3
#include "vk_mem_alloc.h"

#include "RHI/RHI.h"
#include <span>
#include <vector>

namespace he::rhi {

// ============================================================
// VulkanSwapChain — 完整定义（供 Sample 和内部分享）
// ============================================================
class VulkanSwapChain final : public IRHISwapChain {
public:
    VulkanSwapChain(VkDevice device, VkPhysicalDevice physical, VkSurfaceKHR surface,
                    VkQueue presentQueue, const SwapChainDesc& desc);
    ~VulkanSwapChain() override;

    void Resize(u32 width, u32 height) override;
    u32  GetCurrentBackBufferIndex() const override { return m_CurrentImage; }
    u32  GetWidth()  const override { return m_Width; }
    u32  GetHeight() const override { return m_Height; }
    bool AcquireNextImage() override;
    void Present(bool vsync) override;

    VkSwapchainKHR GetHandle()                const { return m_Swapchain; }
    VkFormat       GetFormat()                const { return m_Format; }
    VkImageView    GetImageView(u32 i)        const { return m_ImageViews[i]; }
    VkImageView    GetDepthImageView()        const { return m_DepthImageView; }
    void* GetCurrentBackBufferView() const override { return reinterpret_cast<void*>(m_ImageViews[m_CurrentImage]); }
    void* GetDepthBufferView()       const override { return reinterpret_cast<void*>(m_DepthImageView); }
    VkExtent2D     GetExtent()                const { return {m_Width, m_Height}; }
    VkImage        GetImage(u32 i)            const { return m_Images[i]; }
    VkSemaphore    GetImageAcquiredSemaphore() const { return m_ImageAcquired; }
    VkSemaphore    GetRenderCompleteSemaphore() const { return m_RenderComplete; }

private:
    void CreateSwapchain();
    void DestroySwapchain();

    VkDevice         m_Device        = VK_NULL_HANDLE;
    VkPhysicalDevice m_Physical      = VK_NULL_HANDLE;
    VkSurfaceKHR     m_Surface       = VK_NULL_HANDLE;
    VkQueue          m_PresentQueue  = VK_NULL_HANDLE;

    VkSwapchainKHR   m_Swapchain     = VK_NULL_HANDLE;
    VkFormat         m_Format        = VK_FORMAT_B8G8R8A8_UNORM;
    u32              m_Width         = 0;
    u32              m_Height        = 0;
    u32              m_ImageCount    = 0;
    u32              m_CurrentImage  = 0;

    bool             m_IsMinimized    = false;           // 窗口最小化标记
    VkSemaphore      m_ImageAcquired  = VK_NULL_HANDLE;  // 图像可用信号
    VkSemaphore      m_RenderComplete = VK_NULL_HANDLE;  // 渲染完成信号

    // 深度模板缓冲（与 SwapChain 同尺寸）
    VkImage         m_DepthImage        = VK_NULL_HANDLE;
    VkImageView     m_DepthImageView    = VK_NULL_HANDLE;
    VkDeviceMemory  m_DepthImageMemory  = VK_NULL_HANDLE;

    std::vector<VkImage>     m_Images;
    std::vector<VkImageView> m_ImageViews;
};

// ============================================================
// VulkanCommandList — 完整定义（供 Sample 和内部分享）
// ============================================================
// VulkanDevice 类定义在此处（不在 .cpp 中），因为 VulkanCommandList 需要其完整定义
// 方法实现位于 VulkanDevice.cpp
class VulkanDevice final : public IRHIDevice {
public:
    ~VulkanDevice() override;
    Backend    GetBackend() const override { return Backend::Vulkan; }
    DeviceCaps GetCaps()    const override;

    void Initialize(const DeviceInitDesc& desc) override;
    void Shutdown() override;

    std::unique_ptr<IRHISwapChain>    CreateSwapChain(const SwapChainDesc& desc) override;
    std::unique_ptr<IRHICommandList>  CreateCommandList(QueueType queue) override;
    std::unique_ptr<IRHICommandList>  CreateSecondaryCommandList() override;
    std::unique_ptr<IRHIBuffer>       CreateBuffer(const BufferDesc& desc) override;
    std::unique_ptr<IRHITexture>      CreateTexture(const TextureDesc& desc) override;
    std::unique_ptr<IRHISampler>      CreateSampler(const SamplerDesc& desc) override;
    std::unique_ptr<IRHIPipelineState> CreatePipelineState(const PipelineStateDesc& desc) override;

    void WaitIdle() override;
    void Submit(IRHICommandList* cmdList) override;

    // 多队列支持
    bool HasAsyncComputeQueue() const override;
    u32  GetQueueFamily(QueueType queue) const override;

    // 跨队列同步原语 (Timeline Semaphore)
    RHIFenceHandle CreateFence() override;
    void           DestroyFence(RHIFenceHandle fence) override;
    bool           WaitForFence(RHIFenceHandle fence, u64 value, u64 timeout = UINT64_MAX) override;
    u64            GetFenceValue(RHIFenceHandle fence) const override;
    void           SignalFenceOnQueue(QueueType queue, RHIFenceHandle fence, u64 value) override;
    void           WaitFenceOnQueue(QueueType queue, RHIFenceHandle fence, u64 value) override;
    void           SubmitAll(Span<IRHICommandList*> cmdLists) override;

    // GPU Query
    std::unique_ptr<IRHIQueryPool> CreateQueryPool(u32 queryCount) override;
    float GetTimestampPeriod() override;

    // Descriptor Sets
    DescriptorSetLayoutHandle CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) override;
    DescriptorSetHandle       AllocateDescriptorSet(DescriptorSetLayoutHandle layout) override;
    void                      UpdateDescriptorSet(DescriptorSetHandle set, u32 binding,
                                                  DescriptorType type, IRHIBuffer* buffer) override;
    void                      UpdateDescriptorSet(DescriptorSetHandle set, u32 binding,
                                                  DescriptorType type, IRHITexture* texture,
                                                  IRHISampler* sampler) override;
    void                      UpdateDescriptorSet(DescriptorSetHandle set, u32 binding,
                                                  DescriptorType type,
                                                  IRHITexture** textures, IRHISampler** samplers,
                                                  u32 count) override;
    void                      DestroyDescriptorSetLayout(DescriptorSetLayoutHandle layout) override;

    // Internal
    VkDevice         GetVkDevice()     const { return m_Device; }
    VkPhysicalDevice GetVkPhysical()   const { return m_Physical; }
    VkInstance       GetVkInstance()   const { return m_Instance; }
    VkSurfaceKHR     GetVkSurface()    const { return m_Surface; }
    VkQueue          GetGraphicsQueue() const { return m_GraphicsQueue; }
    VkCommandPool    GetGraphicsCmdPool() const { return m_GraphicsCmdPool; }
    u32              GetGraphicsFamily() const { return m_GraphicsFamily; }
    VmaAllocator     GetVmaAllocator() const { return m_VmaAllocator; }

    // 异步计算队列访问器
    VkQueue GetComputeQueue()  const { return m_ComputeQueue; }
    u32     GetComputeFamily() const { return m_ComputeFamily; }
    bool    HasAsyncCompute()  const { return m_HasAsyncCompute; }

    // Fence 句柄 → VkSemaphore 解析（供 VulkanCommandList 集成 Timeline 信号量）
    VkSemaphore ResolveFenceSemaphore(RHIFenceHandle fence) const {
        if (fence == kInvalidFence || fence > m_Fences.size()) return VK_NULL_HANDLE;
        return m_Fences[static_cast<usize>(fence - 1)].semaphore;
    }

    // Descriptor set handle 解析（供 VulkanCommandList 使用）
    VkDescriptorSet ResolveDescriptorSet(DescriptorSetHandle h) const {
        if (h == 0 || h > m_DescSets.size()) return VK_NULL_HANDLE;
        return m_DescSets[static_cast<usize>(h - 1)];
    }
    VkDescriptorSetLayout ResolveDescriptorSetLayout(DescriptorSetLayoutHandle h) const {
        if (h == 0 || h > m_DescLayoutInfos.size()) return VK_NULL_HANDLE;
        return m_DescLayoutInfos[static_cast<usize>(h - 1)].layout;
    }

private:
    void CreateSurface(void* windowHandle);
    void SelectPhysicalDevice();
    void FindQueueFamilies();     // 查询所有队列族，检测 AsyncCompute 能力
    void CreateLogicalDevice();
    void CreateCommandPools();
    VkSemaphore CreateTimelineSemaphore(u64 initialValue);  // 创建 Timeline 信号量

    VkInstance       m_Instance       = VK_NULL_HANDLE;
    VkPhysicalDevice m_Physical       = VK_NULL_HANDLE;
    VkDevice         m_Device         = VK_NULL_HANDLE;
    VkSurfaceKHR     m_Surface        = VK_NULL_HANDLE;

    VkQueue          m_GraphicsQueue   = VK_NULL_HANDLE;
    u32              m_GraphicsFamily  = 0;

    // 独立 Compute 队列（AsyncCompute）
    VkQueue          m_ComputeQueue    = VK_NULL_HANDLE;
    u32              m_ComputeFamily   = 0;
    bool             m_HasAsyncCompute = false;

    VkCommandPool    m_GraphicsCmdPool = VK_NULL_HANDLE;
    VkCommandPool    m_ComputeCmdPool  = VK_NULL_HANDLE;

    // Timeline Semaphore 池（跨队列同步）
    struct FenceState {
        VkSemaphore semaphore    = VK_NULL_HANDLE;
        u64         currentValue = 0;
    };
    std::vector<FenceState> m_Fences;

    VmaAllocator               m_VmaAllocator     = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;
    bool m_ValidationEnabled = false;

    // Descriptor set management
    VkDescriptorPool                  m_DescPool = VK_NULL_HANDLE;

    /// 描述符集布局信息（含绑定元数据，用于 bindless 分配）
    struct DescLayoutInfo {
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        std::vector<DescriptorSetLayoutBinding> bindings;
        std::vector<VkDescriptorBindingFlags> bindingFlags;
    };
    std::vector<DescLayoutInfo> m_DescLayoutInfos;
    std::vector<VkDescriptorSet>       m_DescSets;
    std::vector<DescriptorSetLayoutHandle> m_DescSetLayoutParents;  // layout handle per set

    void EnsureDescriptorPool();
    VkDescriptorType ToVkDescType(DescriptorType type) const;
};

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
    void SetPushConstants(u32 offset, u32 size, const void* data) override;
    void Dispatch(u32 groupCountX, u32 groupCountY, u32 groupCountZ) override;
    void DispatchIndirect(IRHIBuffer* buffer, u64 offset) override;
    void PipelineBarrier(PipelineStage srcStage, PipelineStage dstStage,
                         ResourceState srcState, ResourceState dstState) override;
    void PipelineBarrier(PipelineStage srcStage, PipelineStage dstStage,
                         ResourceState srcState, ResourceState dstState,
                         IRHITexture* texture) override;
    void CopyBuffer(IRHIBuffer* src, IRHIBuffer* dst,
                    u64 size, u64 srcOffset, u64 dstOffset) override;
    void CopyTextureToTexture(IRHITexture* src, IRHITexture* dst) override;

    // 跨队列所有权转移（AsyncCompute Barrier）
    void QueueOwnershipTransfer(IRHITexture* texture,
                                QueueType srcQueue, QueueType dstQueue,
                                ResourceState currentState, ResourceState newState) override;
    void QueueOwnershipTransfer(IRHIBuffer* buffer,
                                QueueType srcQueue, QueueType dstQueue,
                                ResourceState currentState, ResourceState newState) override;
    void ReleaseToQueue(IRHITexture* texture, QueueType dstQueue) override;
    void AcquireFromQueue(IRHITexture* texture, QueueType srcQueue) override;

    // Timeline Semaphore 集成到 Submit
    void SetTimelineSignal(RHIFenceHandle fence, u64 value) override;
    void SetTimelineWait(RHIFenceHandle fence, u64 value) override;

    // GPU Timestamp Query
    void WriteTimestamp(IRHIQueryPool* pool, u32 queryIndex) override;
    void ResetQueryPool(IRHIQueryPool* pool) override;
    void GetQueryResults(IRHIQueryPool* pool, u32 first, u32 count, u64* data) override;

    void Submit() override;

    // Phase 1 桥接：直接设置 Framebuffer
    void SetSwapchainViews(Span<VkImageView> views, VkExtent2D extent) {
        m_SwapchainViews.assign(views.begin(), views.end());
        m_SwapchainExtent = extent;
    }
    void SetCurrentImageIndex(u32 index) { m_CurrentImageIndex = index; }
    void SetQueueType(QueueType type) { m_QueueType = type; }  // 设置队列类型（跨队列 Barrier 用）
    QueueType GetQueueType() const { return m_QueueType; }      // 获取队列类型

    // 设置 Submit 时的等待/信号 Semaphore
    void SetSyncSemaphores(VkSemaphore wait, VkSemaphore signal) {
        m_WaitSemaphore   = wait;
        m_SignalSemaphore = signal;
    }

    VkCommandBuffer GetHandle() const { return m_CmdBuffers[m_FrameIndex]; }

private:
    VkDevice         m_Device      = VK_NULL_HANDLE;
    VkQueue          m_Queue       = VK_NULL_HANDLE;
    VulkanDevice*    m_VulkanDevice = nullptr;  // 用于 DescriptorSet 句柄解析
    u32              m_QueueFamily = 0;
    QueueType        m_QueueType   = QueueType::Graphics;  // 所属队列类型

    // 三缓冲帧环（Phase 1 多线程渲染）
    static constexpr u32 kMaxFramesInFlight = 3;
    VkCommandPool    m_CmdPools[kMaxFramesInFlight]   = {};
    VkCommandBuffer  m_CmdBuffers[kMaxFramesInFlight] = {};
    VkFence          m_Fences[kMaxFramesInFlight]     = {};
    u32              m_FrameIndex = 0;  // 当前帧槽位

    // Phase 2 辅助命令缓冲池（预分配 6 个，每 Cubemap 面一个，避免重置冲突）
    static constexpr u32 kMaxSecondaryCBs = 3;  // 与 kMaxFramesInFlight 一致，保证 fence 等待后才复用
    VkCommandPool    m_SecondaryPool = VK_NULL_HANDLE;
    VkCommandBuffer  m_SecCmdBuffers[kMaxSecondaryCBs] = {};
    u32              m_SecSlot   = 0;  // 下一个可用槽位
    u32              m_SecActive = 0;  // 当前活跃的 sec CB 索引

    // 关联的 SwapChain（自动管理 Framebuffer + 同步 + 图像索引）
    VulkanSwapChain* m_pSwapChain = nullptr;

    // 同步原语（Binary Semaphore，SwapChain 用）
    VkSemaphore      m_WaitSemaphore   = VK_NULL_HANDLE;
    VkSemaphore      m_SignalSemaphore = VK_NULL_HANDLE;

    // Timeline Semaphore 集成（AsyncCompute 跨队列同步）
    VkSemaphore      m_TimelineSignalSem = VK_NULL_HANDLE;
    u64              m_TimelineSignalVal = 0;
    VkSemaphore      m_TimelineWaitSem   = VK_NULL_HANDLE;
    u64              m_TimelineWaitVal   = 0;

    VkPipeline          m_CurrentPipeline = VK_NULL_HANDLE;
    VkPipelineLayout    m_CurrentLayout   = VK_NULL_HANDLE;
    VkRenderPass        m_CurrentRenderPass = VK_NULL_HANDLE;
    VkRenderPass        m_LoadRenderPass = VK_NULL_HANDLE;  // LOAD_OP 版本（ImGui 叠加用）
    VkRenderPass        m_CurrentFramebufferRP = VK_NULL_HANDLE;  // 追踪 Framebuffer 创建时使用的 RP
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

    // 命令缓冲录制状态（防护 vkCmdBindPipeline 在 Begin 前调用）
    bool          m_IsRecording = false;

    // 离屏渲染通道状态
    VkRenderPass  m_OffscreenRP = VK_NULL_HANDLE;
    bool          m_InOffscreenPass = false;
    // 离屏 FB 延迟销毁（每槽位独立队列，Begin() 仅清理当前槽位）
    VkFramebuffer m_CurrentOffscreenFB = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_PendingFBs[kMaxFramesInFlight];

    // 虚拟深度附件（StartOffscreenPass 无 depth 参数时填充，满足带 depth 的 RenderPass）
    VkImage        m_DummyDepthImage  = VK_NULL_HANDLE;
    VkDeviceMemory m_DummyDepthMemory = VK_NULL_HANDLE;
    VkImageView    m_DummyDepthView   = VK_NULL_HANDLE;
};

// ============================================================
// VulkanPipelineState — 完整定义（析构实现在 VulkanPipeline.cpp）
// ============================================================
class VulkanPipelineState final : public IRHIPipelineState {
public:
    VulkanPipelineState(VkDevice device, VkPipeline pipeline,
                        VkPipelineLayout layout, VkRenderPass renderPass,
                        VkPipelineBindPoint bindPoint)
        : m_Device(device), m_Pipeline(pipeline)
        , m_PipelineLayout(layout), m_RenderPass(renderPass)
        , m_BindPoint(bindPoint) {}
    ~VulkanPipelineState();
    VkPipeline           GetPipeline()       const { return m_Pipeline; }
    VkPipelineLayout     GetPipelineLayout() const { return m_PipelineLayout; }
    VkRenderPass         GetRenderPass()     const { return m_RenderPass; }
    VkPipelineBindPoint  GetBindPoint()      const { return m_BindPoint; }
private:
    VkDevice            m_Device;
    VkPipeline          m_Pipeline;
    VkPipelineLayout    m_PipelineLayout;
    VkRenderPass        m_RenderPass;
    VkPipelineBindPoint m_BindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
};

// ============================================================
// VulkanBuffer — 完整定义（非 inline 方法在 VulkanResources.cpp）
// ============================================================
class VulkanBuffer final : public IRHIBuffer {
public:
    VulkanBuffer(VmaAllocator allocator, const BufferDesc& desc);
    ~VulkanBuffer() override;
    usize GetSize()  const override { return m_Size; }
    void* Map()            override;
    void  Unmap()          override;
    u64   GetDeviceAddress() const override { return m_DeviceAddress; }
    VkBuffer GetHandle() const { return m_Buffer; }
private:
    VmaAllocator      m_Allocator     = VK_NULL_HANDLE;
    VkBuffer          m_Buffer        = VK_NULL_HANDLE;
    VmaAllocation     m_Allocation    = VK_NULL_HANDLE;
    usize             m_Size          = 0;
    u64               m_DeviceAddress = 0;
    bool              m_IsMapped      = false;
    void*             m_MappedPtr     = nullptr;
};

// ============================================================
// VulkanTexture — 完整定义（非 inline 方法在 VulkanResources.cpp）
// ============================================================
class VulkanTexture final : public IRHITexture {
public:
    VulkanTexture(VmaAllocator allocator, VkCommandPool cmdPool, VkQueue queue,
                  const TextureDesc& desc);
    ~VulkanTexture() override;
    u32    GetWidth()        const override { return m_Width; }
    u32    GetHeight()       const override { return m_Height; }
    u32    GetDepth()        const override { return m_Depth; }
    u32    GetMipLevels()    const override { return m_MipLevels; }
    u32    GetArrayLayers()  const override { return m_ArrayLayers; }
    Format GetFormat()       const override { return m_Format; }
    void*  GetNativeHandle() const override { return reinterpret_cast<void*>(m_ImageView); }
    // 逐面句柄（Cubemap face 0-5），非 Cubemap 纹理返回主 ImageView
    void*  GetNativeHandle(u32 index) const override {
        return (index < m_FaceViews.size())
            ? reinterpret_cast<void*>(m_FaceViews[index])
            : reinterpret_cast<void*>(m_ImageView);
    }
    VkImage     GetImage()     const { return m_Image; }
    VkImageView GetImageView() const { return m_ImageView; }
    VkImageView GetFaceView(u32 face) const {
        return (face < m_FaceViews.size()) ? m_FaceViews[face] : VK_NULL_HANDLE;
    }
    VkFormat   GetVkFormat() const { return m_VkFormat; }
    VkDevice   GetDevice()   const { return m_Device; }
private:
    void UploadInitialData(VkCommandPool cmdPool, VkQueue queue, const TextureDesc& desc);
    VkDevice         m_Device       = VK_NULL_HANDLE;
    VmaAllocator     m_Allocator    = VK_NULL_HANDLE;
    VkImage          m_Image        = VK_NULL_HANDLE;
    VkImageView      m_ImageView    = VK_NULL_HANDLE;
    VmaAllocation    m_Allocation   = VK_NULL_HANDLE;
    std::vector<VkImageView> m_FaceViews; // Cubemap 6 面独立 ImageView
    u32              m_Width        = 1;
    u32              m_Height       = 1;
    u32              m_Depth        = 1;
    u32              m_MipLevels    = 1;
    u32              m_ArrayLayers  = 1;
    Format           m_Format       = Format::RGBA8_UNORM;
    VkFormat         m_VkFormat     = VK_FORMAT_R8G8B8A8_UNORM;
};

// ============================================================
// VulkanSampler — 完整定义（非 inline 方法在 VulkanResources.cpp）
// ============================================================
class VulkanSampler final : public IRHISampler {
public:
    VulkanSampler(VkDevice device, const SamplerDesc& desc);
    ~VulkanSampler() override;
    VkSampler GetHandle() const { return m_Sampler; }
private:
    VkDevice   m_Device   = VK_NULL_HANDLE;
    VkSampler  m_Sampler  = VK_NULL_HANDLE;
};

// ============================================================
// VulkanDeviceAccess — 内部桥接：从 IRHIDevice 获取 Vulkan 句柄
// 供 Editor/ImGui 等非 RHI 模块使用
// ============================================================
struct VulkanDeviceAccess {
    static VkInstance       GetInstance(IRHIDevice* d);
    static VkPhysicalDevice GetPhysical(IRHIDevice* d);
    static VkDevice         GetDevice(IRHIDevice* d);
    static u32              GetGraphicsFamily(IRHIDevice* d);
    static VkQueue          GetGraphicsQueue(IRHIDevice* d);
    static VkCommandPool    GetGraphicsCmdPool(IRHIDevice* d);
    static VkQueue          GetComputeQueue(IRHIDevice* d);
    static u32              GetComputeFamily(IRHIDevice* d);
};

// ============================================================
// 格式转换辅助函数（跨 .cpp 共享，实现在 VulkanResources.cpp）
// ============================================================
VkFormat    ToVkFormat(Format fmt);
VkCompareOp ToVkCompareOp(CompareFunc func);

} // namespace he::rhi
