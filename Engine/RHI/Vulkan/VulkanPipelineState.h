#pragma once

// ============================================================
// VulkanPipelineState.h — Vulkan Graphics/Compute Pipeline 状态
// 从 VulkanPipeline.h 拆分（RT 类型已移至 VulkanRT.h）
// ============================================================

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "RHI/Shader.h"

#include <memory>

namespace he::rhi {

// 前向声明
class DeferredDestructionQueue;

// ============================================================
// VulkanPipelineState — Graphics/Compute Pipeline
//
// 支持两种所有权模式：
//   1. 自拥有模式（m_CacheRef == nullptr）：析构时直接销毁 Vulkan 对象
//   2. 缓存模式（m_CacheRef != nullptr）：析构时释放引用，
//      Vulkan 对象由 PSO 缓存通过延迟销毁队列管理生命周期
// ============================================================
class VulkanPipelineState final : public IRHIPipelineState {
public:
    /// 自拥有构造（非缓存模式）
    VulkanPipelineState(VkDevice device, VkPipeline pipeline,
                        VkPipelineLayout layout, VkRenderPass renderPass,
                        VkPipelineBindPoint bindPoint)
        : m_Device(device), m_Pipeline(pipeline)
        , m_PipelineLayout(layout), m_RenderPass(renderPass)
        , m_BindPoint(bindPoint) {}

    /// 缓存共享构造（PSO 缓存模式）
    /// @param cacheRef  共享引用，析构时释放。当最后一个引用释放时，
    ///                  通过 deferredDestroy 将 Vulkan 对象入队延迟销毁
    VulkanPipelineState(VkDevice device, VkPipeline pipeline,
                        VkPipelineLayout layout, VkRenderPass renderPass,
                        VkPipelineBindPoint bindPoint,
                        std::shared_ptr<u32> cacheRef,
                        DeferredDestructionQueue* deferredDestroy)
        : m_Device(device), m_Pipeline(pipeline)
        , m_PipelineLayout(layout), m_RenderPass(renderPass)
        , m_BindPoint(bindPoint)
        , m_CacheRef(std::move(cacheRef))
        , m_DeferredDestroy(deferredDestroy) {}

    ~VulkanPipelineState();
    VkPipeline           GetPipeline()       const { return m_Pipeline; }
    VkPipelineLayout     GetPipelineLayout() const { return m_PipelineLayout; }
    VkRenderPass         GetRenderPass()     const { return m_RenderPass; }
    VkPipelineBindPoint  GetBindPoint()      const { return m_BindPoint; }
    void* GetNativeHandle() const override { return reinterpret_cast<void*>(m_Pipeline); }

    /// 是否来自 PSO 缓存（用于调试和 Hot Reload 失效判断）
    bool IsCached() const { return m_CacheRef != nullptr; }

private:
    VkDevice            m_Device;
    VkPipeline          m_Pipeline;
    VkPipelineLayout    m_PipelineLayout;
    VkRenderPass        m_RenderPass;
    VkPipelineBindPoint m_BindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;

    // PSO 缓存引用：非空时表示 Vulkan 对象由缓存管理生命周期
    std::shared_ptr<u32>              m_CacheRef;
    DeferredDestructionQueue*         m_DeferredDestroy = nullptr;
};

} // namespace he::rhi
