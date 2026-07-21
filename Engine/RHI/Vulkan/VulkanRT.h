#pragma once

// ============================================================
// VulkanRT.h — Ray Tracing 类型与辅助函数
//
// 包含：
//   VulkanRTDispatch            — RT 扩展函数派发表
//   VulkanAccelerationStructure — BLAS/TLAS 加速结构
//   VulkanRTPipelineState       — RT 管线状态 + 着色器组句柄
//   共享辅助函数                 — ToVkFormat / ToVkCompareOp / ToVkBuildFlags
// ============================================================

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

// VMA（AS 底层存储缓冲用）
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
// 共享辅助函数声明（跨编译单元）
//   ToVkFormat     — VulkanResources.cpp
//   ToVkCompareOp  — VulkanPipeline.cpp
//   ToVkBuildFlags — VulkanDevice.cpp / VulkanRT.cpp
// ============================================================
VkFormat    ToVkFormat(Format fmt);
VkCompareOp ToVkCompareOp(CompareFunc func);
VkAttachmentLoadOp ToVkLoadOp(LoadOp op);
VkBuildAccelerationStructureFlagsKHR ToVkBuildFlags(ASBuildFlags flags);

// 跨平台渲染状态 → Vulkan 转换
VkCullModeFlags   ToVkCullMode(CullMode mode);
VkFrontFace       ToVkFrontFace(FrontFace face);
VkPolygonMode     ToVkFillMode(FillMode mode);
VkBlendFactor     ToVkBlendFactor(BlendFactor factor);
VkBlendOp         ToVkBlendOp(BlendOp op);
VkColorComponentFlags ToVkColorWriteMask(ColorWriteMask mask);

} // namespace he::rhi
