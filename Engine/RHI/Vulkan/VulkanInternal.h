#pragma once

// ============================================================
// VulkanInternal.h — Internal bridge for sample/testing code
//
// NOT part of the public RHI API. Used only by samples
// until the full RHI abstraction is complete (Phase 1-2).
// ============================================================

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "RHI/RHI.h"
#include <span>
#include <vector>

namespace he::rhi {

// Exported internal types for Vulkan backend access
// Forward declared here, defined in VulkanDevice.cpp / VulkanResources.cpp

class VulkanSwapChain : public IRHISwapChain {
public:
    VkSwapchainKHR GetHandle()     const;
    VkImageView    GetImageView(u32 i) const;
};

class VulkanCommandList : public IRHICommandList {
public:
    void SetSwapchainViews(std::span<VkImageView> views, VkExtent2D extent);
    void SetCurrentImageIndex(u32 index);
};

class VulkanPipelineState : public IRHIPipelineState {
public:
    VkPipeline       GetPipeline()       const;
    VkPipelineLayout GetPipelineLayout() const;
    VkRenderPass     GetRenderPass()     const;
};

class VulkanBuffer : public IRHIBuffer {
public:
    VkBuffer GetHandle() const;
};

} // namespace he::rhi
