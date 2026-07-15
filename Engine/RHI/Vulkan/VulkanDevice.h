#pragma once

// ============================================================
// VulkanDevice.h — VulkanDevice 类 + 辅助类型
//
// 由 VulkanInternal.h 拆分而来，包含子模块头文件：
//   VulkanSwapChain.h   — VulkanSwapChain
//   VulkanCommandList.h — VulkanCommandList
//   VulkanResources.h   — VulkanBuffer / VulkanTexture / VulkanSampler
//   VulkanPipeline.h    — VulkanPipelineState / AS / RT Pipeline
//
// 外部使用者可 include 此顶层头文件获取全部 Vulkan 内部类型，
// 或按需 include 子模块头文件。
// ============================================================

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

// VMA (Vulkan Memory Allocator)
#define VMA_VULKAN_VERSION 1003000
#include "vk_mem_alloc.h"

#include "RHI/RHI.h"
#include "VulkanDGC.h"

// 子模块头文件
#include "VulkanSwapChain.h"
#include "VulkanCommandList.h"
#include "VulkanResources.h"
#include "VulkanRT.h"
#include "VulkanPipelineState.h"

#include <span>
#include <vector>

namespace he::rhi {

// ============================================================
// VulkanDevice — Vulkan 逻辑设备封装
// ============================================================
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

    // Ray Tracing 资源创建
    std::unique_ptr<IRHIAccelerationStructure>
        CreateBLAS(const BLASBuildDesc& desc) override;
    std::unique_ptr<IRHIAccelerationStructure>
        CreateTLAS(const TLASBuildDesc& desc) override;
    ASBuildSizes GetBLASBuildSizes(const BLASBuildDesc& desc) override;
    ASBuildSizes GetTLASBuildSizes(u32 maxInstanceCount) override;
    std::unique_ptr<IRHIRayTracingPipelineState>
        CreateRTPipelineState(const RTPipelineStateDesc& desc) override;

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
    void                      UpdateDescriptorSetWithImageView(DescriptorSetHandle set, u32 binding,
                                                                DescriptorType type, void* imageView) override;
    void                      UpdateDescriptorSet(DescriptorSetHandle set, u32 binding,
                                                  DescriptorType type,
                                                  IRHIAccelerationStructure* as) override;

    // Per-Mip ImageView 支持
    void*                     CreateTextureMipStorageView(IRHITexture* texture, u32 mipLevel) override;
    void*                     CreateTextureMipSampledView(IRHITexture* texture, u32 mipLevel) override;
    void                      DestroyTextureMipView(void* view) override;

    // Internal 访问器
    VkDevice         GetVkDevice()     const { return m_Device; }
    VkPhysicalDevice GetVkPhysical()   const { return m_Physical; }
    VkInstance       GetVkInstance()   const { return m_Instance; }
    VkSurfaceKHR     GetVkSurface()    const { return m_Surface; }
    VkQueue          GetGraphicsQueue() const { return m_GraphicsQueue; }
    VkCommandPool    GetGraphicsCmdPool() const { return m_GraphicsCmdPool; }
    u32              GetGraphicsFamily() const { return m_GraphicsFamily; }
    VmaAllocator     GetVmaAllocator() const { return m_VmaAllocator; }

    VkQueue GetComputeQueue()  const { return m_ComputeQueue; }
    u32     GetComputeFamily() const { return m_ComputeFamily; }
    bool    HasAsyncCompute()  const { return m_HasAsyncCompute; }

    // RT / Mesh / DGC 支持状态
    bool    SupportsRayTracing()  const { return m_SupportsRT; }
    bool    SupportsMeshShaders() const { return m_SupportsMesh; }
    bool    SupportsDGC() const { return m_SupportsDGC; }
    const VulkanDGCFuncs& GetDGCFuncs() const { return m_DGCFuncs; }

    // Shader Group 句柄信息（SBT 构建用）
    u32 GetShaderGroupHandleSize()    const { return m_ShaderGroupHandleSize; }
    u32 GetShaderGroupBaseAlignment() const { return m_ShaderGroupBaseAlignment; }

    // RT 函数派发表
    VulkanRTDispatch& GetRTDispatch() { return m_RT; }

    // Mesh Shader 函数指针
    PFN_vkCmdDrawMeshTasksEXT          m_CmdDrawMeshTasks         = nullptr;
    PFN_vkCmdDrawMeshTasksIndirectEXT  m_CmdDrawMeshTasksIndirect = nullptr;

    // Fence → VkSemaphore 解析
    VkSemaphore ResolveFenceSemaphore(RHIFenceHandle fence) const {
        if (fence == kInvalidFence || fence > m_Fences.size()) return VK_NULL_HANDLE;
        return m_Fences[static_cast<usize>(fence - 1)].semaphore;
    }

    // DescriptorSet handle 解析
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
    void FindQueueFamilies();
    void CreateLogicalDevice();
    void CreateCommandPools();
    VkSemaphore CreateTimelineSemaphore(u64 initialValue);
    void QueryRTCapabilities();
    void QueryMeshCapabilities();
    void QueryDGCCapabilities();
    void LoadDGCFunctions();
    void LoadRTFunctions();
    void LoadMeshFunctions();

    VkInstance       m_Instance       = VK_NULL_HANDLE;
    VkPhysicalDevice m_Physical       = VK_NULL_HANDLE;
    VkDevice         m_Device         = VK_NULL_HANDLE;
    VkSurfaceKHR     m_Surface        = VK_NULL_HANDLE;

    VkQueue          m_GraphicsQueue   = VK_NULL_HANDLE;
    u32              m_GraphicsFamily  = 0;

    VkQueue          m_ComputeQueue    = VK_NULL_HANDLE;
    u32              m_ComputeFamily   = 0;
    bool             m_HasAsyncCompute = false;

    // RT / Mesh / DGC 支持状态 + 硬件属性
    bool             m_SupportsRT              = false;
    bool             m_SupportsRTPositionFetch = false;
    bool             m_SupportsMesh            = false;
    bool             m_SupportsDGC             = false;
    // RT Pipeline 属性
    u32              m_MaxRayRecursionDepth     = 1;
    u32              m_ShaderGroupHandleSize    = 32;
    u32              m_ShaderGroupBaseAlignment = 64;
    u64              m_MaxRTDispatchSize        = 0;
    // AS 属性
    u64              m_MaxASInstanceCount       = 0;
    u64              m_MaxASGeometryCount       = 0;
    u64              m_MaxASPrimitiveCount      = 0;
    u64              m_MinASScratchAlignment    = 0;
    // Mesh Shader 属性
    u32              m_MaxMeshWorkGroupInvocations = 128;
    u32              m_MaxMeshOutputVertices       = 256;
    u32              m_MaxMeshOutputPrimitives     = 256;
    u32              m_MaxTaskWorkGroupInvocations = 128;
    u32              m_MaxTaskPayloadSize          = 16384;
    u32              m_MaxMeshWorkGroupCountX      = 65535;
    u32              m_MaxMeshWorkGroupCountY      = 65535;
    u32              m_MaxMeshWorkGroupCountZ      = 65535;

    VkCommandPool    m_GraphicsCmdPool = VK_NULL_HANDLE;
    VkCommandPool    m_ComputeCmdPool  = VK_NULL_HANDLE;

    // Timeline Semaphore 池
    struct FenceState {
        VkSemaphore semaphore    = VK_NULL_HANDLE;
        u64         currentValue = 0;
    };
    std::vector<FenceState> m_Fences;

    VmaAllocator               m_VmaAllocator     = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;
    bool m_ValidationEnabled = false;

    VulkanRTDispatch m_RT;
    VulkanDGCFuncs   m_DGCFuncs;

    VkDescriptorPool                  m_DescPool = VK_NULL_HANDLE;

    struct DescLayoutInfo {
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        std::vector<DescriptorSetLayoutBinding> bindings;
        std::vector<VkDescriptorBindingFlags> bindingFlags;
    };
    std::vector<DescLayoutInfo> m_DescLayoutInfos;
    std::vector<VkDescriptorSet>       m_DescSets;
    std::vector<DescriptorSetLayoutHandle> m_DescSetLayoutParents;

    void EnsureDescriptorPool();
    VkDescriptorType ToVkDescType(DescriptorType type) const;
};

// ============================================================
// VulkanDeviceAccess — 内部桥接：从 IRHIDevice 获取 Vulkan 句柄
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

} // namespace he::rhi
