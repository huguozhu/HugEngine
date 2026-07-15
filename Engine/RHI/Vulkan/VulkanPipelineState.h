#pragma once

// ============================================================
// VulkanPipelineState.h — Vulkan Graphics/Compute Pipeline 状态
// 从 VulkanPipeline.h 拆分（RT 类型已移至 VulkanRT.h）
// ============================================================

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "RHI/Shader.h"

namespace he::rhi {

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

} // namespace he::rhi
