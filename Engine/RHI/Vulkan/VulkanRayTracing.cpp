// ============================================================
// VulkanRayTracing.cpp — Vulkan Ray Tracing 资源实现
// 负责 AccelerationStructure (BLAS/TLAS) + RT Pipeline State
// ============================================================
#include "RHI/RHI.h"
#include "Core/Log.h"
#include "Core/Assert.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <cstring>

// Vulkan 类型的完整定义
#include "VulkanInternal.h"

namespace he::rhi {

// ============================================================
// VulkanAccelerationStructure 实现
// ============================================================

VulkanAccelerationStructure::VulkanAccelerationStructure(
    VkDevice device, VmaAllocator allocator,
    AccelerationStructureType type, const VulkanRTDispatch& rt, u64 asSize)
    : m_Device(device), m_Allocator(allocator), m_Type(type), m_Size(asSize)
{
    // 1. 创建底层存储缓冲（DEVICE_LOCAL，GPU 端读写）
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size  = asSize;
    bufferInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
                     | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;  // 优先 DEVICE_LOCAL

    VkResult result = vmaCreateBuffer(allocator, &bufferInfo, &allocInfo,
                                       &m_Buffer, &m_Allocation, nullptr);
    HE_ASSERT(result == VK_SUCCESS, "VMA: 创建 AS 存储缓冲失败");

    // 获取底层缓冲 GPU 地址（用于 TLAS 实例引用）
    VkBufferDeviceAddressInfo addrInfo{};
    addrInfo.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addrInfo.buffer = m_Buffer;
    m_BufferAddress = vkGetBufferDeviceAddress(device, &addrInfo);

    // 2. 创建 VkAccelerationStructureKHR
    VkAccelerationStructureCreateInfoKHR asInfo{};
    asInfo.sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    asInfo.buffer = m_Buffer;
    asInfo.size   = asSize;
    asInfo.type   = (type == AccelerationStructureType::BottomLevel)
                    ? VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR
                    : VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;

    result = rt.createAS(device, &asInfo, nullptr, &m_AS);
    HE_ASSERT(result == VK_SUCCESS, "创建 VkAccelerationStructureKHR 失败");

    // 3. 获取 AS GPU 地址（供 TLAS 实例引用 + TraceRays 使用）
    VkAccelerationStructureDeviceAddressInfoKHR asAddrInfo{};
    asAddrInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    asAddrInfo.accelerationStructure = m_AS;
    m_DeviceAddress = rt.getASDeviceAddress(device, &asAddrInfo);

    HE_CORE_INFO("VulkanAccelerationStructure: type={} size={}MB deviceAddr={:#x}",
                 (type == AccelerationStructureType::BottomLevel) ? "BLAS" : "TLAS",
                 asSize / (1024 * 1024), m_DeviceAddress);
}

VulkanAccelerationStructure::~VulkanAccelerationStructure() {
    if (m_AS) {
        // 使用存储的派发函数指针销毁 AS（设备可能在析构时仍有效）
        // 注意: 此处假设 VulkanDevice 析构在 AS 析构之后，m_Device 仍有效
        vkDestroyAccelerationStructureKHR(m_Device, m_AS, nullptr);
    }
    if (m_Buffer) {
        vmaDestroyBuffer(m_Allocator, m_Buffer, m_Allocation);
    }
}

// ============================================================
// VulkanRTPipelineState 实现
// ============================================================

VulkanRTPipelineState::VulkanRTPipelineState(
    VkDevice device, VkPipeline pipeline, VkPipelineLayout layout,
    u32 groupCount, u32 handleSize, std::vector<u8> handles)
    : m_Device(device), m_Pipeline(pipeline), m_PipelineLayout(layout)
    , m_GroupCount(groupCount), m_HandleSize(handleSize)
    , m_Handles(std::move(handles))
{
    HE_CORE_INFO("VulkanRTPipelineState: {} groups, handleSize={}, totalHandles={}B",
                 groupCount, handleSize, m_Handles.size());
}

VulkanRTPipelineState::~VulkanRTPipelineState() {
    if (m_Pipeline)       vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
    if (m_PipelineLayout) vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
}

} // namespace he::rhi
