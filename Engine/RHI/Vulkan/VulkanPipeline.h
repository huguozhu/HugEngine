#pragma once

// ============================================================
// VulkanPipeline.h — Vulkan Pipeline / AS / RT Pipeline 类型
// 从 VulkanInternal.h 拆分
// ============================================================

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

// VMA (VulkanBuffer/AS 底层存储用)
#define VMA_VULKAN_VERSION 1003000
#include "vk_mem_alloc.h"

#include "RHI/Shader.h"
#include "RHI/RayTracing.h"

#include <vector>

namespace he::rhi {

// ============================================================
// VulkanRTDispatch — RT 扩展函数派发表
// ============================================================
struct VulkanRTDispatch {
    PFN_vkCreateAccelerationStructureKHR           createAS              = nullptr;
    PFN_vkDestroyAccelerationStructureKHR          destroyAS             = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR    getASBuildSizes       = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR        cmdBuildAS            = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR getASDeviceAddress    = nullptr;
    PFN_vkCreateRayTracingPipelinesKHR             createRTPipelines     = nullptr;
    PFN_vkGetRayTracingShaderGroupHandlesKHR       getRTShaderGroupHandles = nullptr;
    PFN_vkCmdTraceRaysKHR                          cmdTraceRays          = nullptr;
};

// ============================================================
// VulkanPipelineState — Graphics/Compute Pipeline
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
    void* GetNativeHandle() const override { return reinterpret_cast<void*>(m_Pipeline); }
private:
    VkDevice            m_Device;
    VkPipeline          m_Pipeline;
    VkPipelineLayout    m_PipelineLayout;
    VkRenderPass        m_RenderPass;
    VkPipelineBindPoint m_BindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
};

// ============================================================
// VulkanAccelerationStructure — BLAS/TLAS 加速结构
// ============================================================
class VulkanAccelerationStructure final : public IRHIAccelerationStructure {
public:
    VulkanAccelerationStructure(VkDevice device, VmaAllocator allocator,
                                AccelerationStructureType type,
                                const VulkanRTDispatch& rt,
                                u64 asSize);
    ~VulkanAccelerationStructure() override;

    u64 GetDeviceAddress() const override { return m_DeviceAddress; }
    u64 GetSize()           const override { return m_Size; }

    VkAccelerationStructureKHR GetHandle()    const { return m_AS; }
    AccelerationStructureType  GetType()       const { return m_Type; }
    VkDevice                  GetDevice()     const { return m_Device; }
    u64                       GetBufferAddress() const { return m_BufferAddress; }

    void SetBLASDesc(const BLASBuildDesc& desc) { m_BLASDesc = desc; }
    const BLASBuildDesc& GetBLASDesc() const { return m_BLASDesc; }

private:
    VkDevice                    m_Device        = VK_NULL_HANDLE;
    VmaAllocator                m_Allocator     = VK_NULL_HANDLE;
    VkAccelerationStructureKHR  m_AS            = VK_NULL_HANDLE;
    VkBuffer                    m_Buffer        = VK_NULL_HANDLE;
    VmaAllocation               m_Allocation    = VK_NULL_HANDLE;
    u64                         m_DeviceAddress = 0;
    u64                         m_BufferAddress = 0;
    u64                         m_Size          = 0;
    AccelerationStructureType   m_Type          = AccelerationStructureType::BottomLevel;
    BLASBuildDesc               m_BLASDesc;
    PFN_vkDestroyAccelerationStructureKHR m_DestroyAS = nullptr;
};

// ============================================================
// VulkanRTPipelineState — RT 管线状态 + 着色器组句柄
// ============================================================
class VulkanRTPipelineState final : public IRHIRayTracingPipelineState {
public:
    VulkanRTPipelineState(VkDevice device, VkPipeline pipeline,
                          VkPipelineLayout layout, u32 groupCount,
                          u32 handleSize, std::vector<u8> handles);
    ~VulkanRTPipelineState() override;

    u32 GetShaderGroupCount()       const override { return m_GroupCount; }
    u32 GetShaderGroupHandleSize()  const override { return m_HandleSize; }
    std::vector<u8> GetShaderGroupHandles() const override { return m_Handles; }

    VkPipeline       GetPipeline()       const { return m_Pipeline; }
    VkPipelineLayout GetPipelineLayout() const { return m_PipelineLayout; }

private:
    VkDevice            m_Device         = VK_NULL_HANDLE;
    VkPipeline          m_Pipeline       = VK_NULL_HANDLE;
    VkPipelineLayout    m_PipelineLayout = VK_NULL_HANDLE;
    u32                 m_GroupCount     = 0;
    u32                 m_HandleSize     = 0;
    std::vector<u8>     m_Handles;
};

// ============================================================
// 共享辅助函数声明（跨编译单元，实现在 VulkanResources.cpp + VulkanDevice.cpp）
// ============================================================
VkFormat    ToVkFormat(Format fmt);
VkCompareOp ToVkCompareOp(CompareFunc func);
VkBuildAccelerationStructureFlagsKHR ToVkBuildFlags(ASBuildFlags flags);

} // namespace he::rhi
