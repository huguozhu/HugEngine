#pragma once

#include "RHI/QueryPool.h"
#include <vulkan/vulkan.h>

namespace he::rhi {

class VulkanQueryPool final : public IRHIQueryPool {
public:
    VulkanQueryPool(VkDevice device, u32 queryCount)
        : m_Device(device), m_Count(queryCount) {
        VkQueryPoolCreateInfo info{};
        info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = queryCount;
        vkCreateQueryPool(device, &info, nullptr, &m_Pool);
    }
    ~VulkanQueryPool() override { vkDestroyQueryPool(m_Device, m_Pool, nullptr); }

    u32 GetQueryCount() const override { return m_Count; }
    VkQueryPool GetHandle() const { return m_Pool; }

private:
    VkDevice   m_Device = VK_NULL_HANDLE;
    VkQueryPool m_Pool   = VK_NULL_HANDLE;
    u32        m_Count  = 0;
};

} // namespace he::rhi
