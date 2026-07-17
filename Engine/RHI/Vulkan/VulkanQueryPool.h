#pragma once

#include "RHI/QueryPool.h"
#include <vulkan/vulkan.h>

namespace he::rhi {

// ============================================================
// VulkanQueryPool — GPU 查询池（Timestamp + PipelineStatistics）
// ============================================================
class VulkanQueryPool final : public IRHIQueryPool {
public:
    VulkanQueryPool(VkDevice device, u32 queryCount, QueryType type = QueryType::Timestamp)
        : m_Device(device), m_Count(queryCount), m_Type(type) {
        VkQueryPoolCreateInfo info{};
        info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryCount = queryCount;

        if (type == QueryType::PipelineStatistics) {
            info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
            // 选择常用的 6 个管线统计计数器
            info.pipelineStatistics =
                VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT    |
                VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT  |
                VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT  |
                VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT       |
                VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT        |
                VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        } else {
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        }

        vkCreateQueryPool(device, &info, nullptr, &m_Pool);
    }
    ~VulkanQueryPool() override { vkDestroyQueryPool(m_Device, m_Pool, nullptr); }

    u32       GetQueryCount() const override { return m_Count; }
    QueryType GetQueryType()  const override { return m_Type; }
    VkQueryPool GetHandle()   const         { return m_Pool; }

private:
    VkDevice    m_Device = VK_NULL_HANDLE;
    VkQueryPool m_Pool   = VK_NULL_HANDLE;
    u32         m_Count  = 0;
    QueryType   m_Type   = QueryType::Timestamp;
};

} // namespace he::rhi
