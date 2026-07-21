// VulkanCommandList_RenderPass.cpp — BeginRenderPass / OffscreenPass 渲染通道管理
// 从 VulkanCommandList.cpp 拆分

#include "RHI/RHI.h"
#include "RHI/SwapChain.h"
#include "Core/Log.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "VulkanCommandList.h"
#include "VulkanDevice.h"

namespace he::rhi {

// ============================================================
// BeginRenderPass — SwapChain 渲染目标
// ============================================================

void VulkanCommandList::BeginRenderPass(u32 colorCount, Format, Format depthFormat,
                                        const ClearValue* clear, LoadOp loadOp) {
    // 从 SwapChain 获取当前图像索引
    if (m_pSwapChain)
        m_CurrentImageIndex = m_pSwapChain->GetCurrentBackBufferIndex();

    if (m_SwapchainViews.empty() || !m_CurrentRenderPass) {
        HE_CORE_ERROR("BeginRenderPass: no swapchain views or render pass set");
        return;
    }

    // 先确定最终使用的 RenderPass（LoadOp 不同 → RP 不同）
    VkRenderPass rp = m_CurrentRenderPass;
    if (loadOp == LoadOp::Load) {
        if (m_LoadRenderPass == VK_NULL_HANDLE) {
            // 懒创建 LOAD 版本 RenderPass（保留 BackBuffer 内容 + 深度匹配 PSO RP 的 finalLayout）
            VkAttachmentDescription att[2]{};
            att[0].format = VK_FORMAT_B8G8R8A8_UNORM; // SwapChain 格式
            att[0].samples = VK_SAMPLE_COUNT_1_BIT;
            att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // 保留 BackBuffer 内容
            att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            att[0].initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            att[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            att[1].format = VK_FORMAT_D32_SFLOAT;
            att[1].samples = VK_SAMPLE_COUNT_1_BIT;
            att[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            att[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

            VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &colorRef;
            subpass.pDepthStencilAttachment = &depthRef;

            VkSubpassDependency dep{};
            dep.srcSubpass = VK_SUBPASS_EXTERNAL;
            dep.dstSubpass = 0;
            // 与 PSO 的 RenderPass 保持一致（含深度附件依赖）
            dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                             | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                             | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                              | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

            VkRenderPassCreateInfo rpInfo{};
            rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            rpInfo.attachmentCount = 2;
            rpInfo.pAttachments = att;
            rpInfo.subpassCount = 1;
            rpInfo.pSubpasses = &subpass;
            rpInfo.dependencyCount = 1;
            rpInfo.pDependencies = &dep;
            vkCreateRenderPass(m_Device, &rpInfo, nullptr, &m_LoadRenderPass);
        }
        rp = m_LoadRenderPass;
    }

    // RP 变化时强制重建 Framebuffer（确保 FB 与 RP 兼容）
    if (rp != m_CurrentFramebufferRP) {
        m_FramebuffersNeedRebuild = true;
        m_CurrentFramebufferRP = rp;
    }

    // 创建 Framebuffer（颜色 + 深度附件），使用最终 RP
    if (m_Framebuffers.empty() || m_FramebuffersNeedRebuild) {
        if (!m_Framebuffers.empty()) {
            m_Framebuffers.clear();
        }
        m_FramebuffersNeedRebuild = false;
        u32 count = static_cast<u32>(m_SwapchainViews.size());
        m_Framebuffers.resize(count);
        for (u32 i = 0; i < count; ++i) {
            VkImageView attachments[2] = { m_SwapchainViews[i] };
            u32 attachmentCount = 1;

            if (m_pSwapChain && m_pSwapChain->GetDepthImageView()) {
                attachments[1] = m_pSwapChain->GetDepthImageView();
                attachmentCount = 2;
            }

            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass      = rp;
            fbInfo.attachmentCount = attachmentCount;
            fbInfo.pAttachments    = attachments;
            fbInfo.width           = m_SwapchainExtent.width;
            fbInfo.height          = m_SwapchainExtent.height;
            fbInfo.layers          = 1;

            vkCreateFramebuffer(m_Device, &fbInfo, nullptr, &m_Framebuffers[i]);
        }
    }

    // 构建清除值（颜色 + 深度，始终 2 个以匹配 RenderPass 附件数）
    VkClearValue vkClearValues[2]{};
    if (clear) {
        vkClearValues[0].color.float32[0] = clear[0].color[0];
        vkClearValues[0].color.float32[1] = clear[0].color[1];
        vkClearValues[0].color.float32[2] = clear[0].color[2];
        vkClearValues[0].color.float32[3] = clear[0].color[3];
    } else {
        vkClearValues[0].color.float32[0] = 0.1f;
        vkClearValues[0].color.float32[1] = 0.1f;
        vkClearValues[0].color.float32[2] = 0.15f;
        vkClearValues[0].color.float32[3] = 1.0f;
    }
    vkClearValues[1].depthStencil.depth   = 1.0f;
    vkClearValues[1].depthStencil.stencil = 0;

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass        = rp;
    rpBegin.framebuffer       = m_Framebuffers[m_CurrentImageIndex];
    rpBegin.renderArea.extent = m_SwapchainExtent;
    rpBegin.clearValueCount   = 2;
    rpBegin.pClearValues      = vkClearValues;

    vkCmdBeginRenderPass(m_CmdBuffers[m_FrameIndex], &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    // 绑定管线
    if (m_CurrentPipeline) {
        vkCmdBindPipeline(m_CmdBuffers[m_FrameIndex], VK_PIPELINE_BIND_POINT_GRAPHICS, m_CurrentPipeline);
    }
}

void VulkanCommandList::EndRenderPass() {
    vkCmdEndRenderPass(m_CmdBuffers[m_FrameIndex]);
}

// ============================================================
// 离屏渲染通道（非 SwapChain 渲染目标，用于阴影贴图等）
// ============================================================

void VulkanCommandList::BeginOffscreenPass(
    void* colorImageView, void* depthImageView,
    u32 width, u32 height, const ClearValue* clear, bool allowSecondary)
{
    auto colorView = static_cast<VkImageView>(colorImageView);
    auto depthView = static_cast<VkImageView>(depthImageView);
    if (!colorView && !depthView) {
        HE_CORE_ERROR("BeginOffscreenPass: 至少需要一个附件");
        return;
    }
    if (m_CurrentRenderPass == VK_NULL_HANDLE) {
        HE_CORE_ERROR("BeginOffscreenPass: 未设置 PSO（先调用 SetPipeline）");
        return;
    }

    VkImageView attachments[2] = {};
    u32 attachmentCount = 0;
    if (colorView) attachments[attachmentCount++] = colorView;
    if (depthView) attachments[attachmentCount++] = depthView;

    VkFramebuffer offscreenFB = VK_NULL_HANDLE;
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass      = m_CurrentRenderPass;
    fbInfo.attachmentCount = attachmentCount;
    fbInfo.pAttachments    = attachments;
    fbInfo.width           = width;
    fbInfo.height          = height;
    fbInfo.layers          = 1;
    vkCreateFramebuffer(m_Device, &fbInfo, nullptr, &offscreenFB);
    m_CurrentOffscreenFB = offscreenFB;

    VkClearValue vkClearValues[2]{};
    u32 clearCount = 0;
    if (colorView) {
        if (clear) {
            vkClearValues[clearCount].color.float32[0] = clear->color[0];
            vkClearValues[clearCount].color.float32[1] = clear->color[1];
            vkClearValues[clearCount].color.float32[2] = clear->color[2];
            vkClearValues[clearCount].color.float32[3] = clear->color[3];
        }
        clearCount++;
    }
    if (depthView) {
        vkClearValues[clearCount].depthStencil.depth   = clear ? clear->depth   : 1.0f;
        vkClearValues[clearCount].depthStencil.stencil = clear ? clear->stencil : 0;
        clearCount++;
    }

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass  = m_CurrentRenderPass;
    rpBegin.framebuffer = offscreenFB;
    rpBegin.renderArea.extent = { width, height };
    rpBegin.clearValueCount   = clearCount;
    rpBegin.pClearValues      = vkClearValues;

    VkSubpassContents contents = allowSecondary
        ? VK_SUBPASS_CONTENTS_INLINE_AND_SECONDARY_COMMAND_BUFFERS_KHR
        : VK_SUBPASS_CONTENTS_INLINE;
    vkCmdBeginRenderPass(m_CmdBuffers[m_FrameIndex], &rpBegin, contents);
    m_InOffscreenPass = true;

    if (m_CurrentPipeline != VK_NULL_HANDLE) {
        vkCmdBindPipeline(m_CmdBuffers[m_FrameIndex], VK_PIPELINE_BIND_POINT_GRAPHICS, m_CurrentPipeline);
    }
}

void VulkanCommandList::BeginOffscreenPassMRT(
    void* const* colorImageViews, u32 colorCount,
    void* depthImageView, u32 width, u32 height,
    const ClearValue* clears, bool allowSecondary)
{
    if (colorCount == 0 && !depthImageView) {
        HE_CORE_ERROR("BeginOffscreenPassMRT: 至少需要一个附件");
        return;
    }
    if (m_CurrentRenderPass == VK_NULL_HANDLE) {
        HE_CORE_ERROR("BeginOffscreenPassMRT: 未设置 PSO（先调用 SetPipeline）");
        return;
    }

    // 构建附件列表：颜色在前，深度在后；支持最多 7 个颜色 + 1 个深度（共 8 个）
    VkImageView attachments[kMaxColorAttachments] = {};
    u32 attachmentCount = 0;
    for (u32 i = 0; i < colorCount && attachmentCount < 7; ++i)
        attachments[attachmentCount++] = static_cast<VkImageView>(colorImageViews[i]);
    auto depthView = static_cast<VkImageView>(depthImageView);
    u32 depthIndex = attachmentCount;
    if (depthView) attachments[attachmentCount++] = depthView;

    VkFramebuffer offscreenFB = VK_NULL_HANDLE;
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass      = m_CurrentRenderPass;
    fbInfo.attachmentCount = attachmentCount;
    fbInfo.pAttachments    = attachments;
    fbInfo.width           = width;
    fbInfo.height          = height;
    fbInfo.layers          = 1;
    vkCreateFramebuffer(m_Device, &fbInfo, nullptr, &offscreenFB);
    m_CurrentOffscreenFB = offscreenFB;

    // 清除值（最多 7 个颜色 + 1 个深度，共 8 个）
    VkClearValue vkClearValues[kMaxColorAttachments]{};
    u32 clearCount = 0;
    for (u32 i = 0; i < colorCount; ++i) {
        if (clears) {
            vkClearValues[clearCount].color.float32[0] = clears[i].color[0];
            vkClearValues[clearCount].color.float32[1] = clears[i].color[1];
            vkClearValues[clearCount].color.float32[2] = clears[i].color[2];
            vkClearValues[clearCount].color.float32[3] = clears[i].color[3];
        }
        clearCount++;
    }
    if (depthView) {
        vkClearValues[clearCount].depthStencil.depth = clears ? clears[colorCount].depth : 1.0f;
        vkClearValues[clearCount].depthStencil.stencil = 0;
        clearCount++;
    }

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass  = m_CurrentRenderPass;
    rpBegin.framebuffer = offscreenFB;
    rpBegin.renderArea.extent = { width, height };
    rpBegin.clearValueCount   = clearCount;
    rpBegin.pClearValues      = vkClearValues;

    VkSubpassContents contents = allowSecondary
        ? VK_SUBPASS_CONTENTS_INLINE_AND_SECONDARY_COMMAND_BUFFERS_KHR
        : VK_SUBPASS_CONTENTS_INLINE;
    vkCmdBeginRenderPass(m_CmdBuffers[m_FrameIndex], &rpBegin, contents);
    m_InOffscreenPass = true;

    if (m_CurrentPipeline != VK_NULL_HANDLE) {
        vkCmdBindPipeline(m_CmdBuffers[m_FrameIndex], VK_PIPELINE_BIND_POINT_GRAPHICS, m_CurrentPipeline);
    }
}

void VulkanCommandList::EndOffscreenPass() {
    if (!m_InOffscreenPass) return;
    vkCmdEndRenderPass(m_CmdBuffers[m_FrameIndex]);
    m_InOffscreenPass = false;

    // FB 不能立即销毁 — CB 尚未提交。
    // 通过 VulkanDevice 的延迟销毁队列统一管理，3 帧后安全销毁。
    if (m_CurrentOffscreenFB) {
        VkDevice dev = m_Device;
        VkFramebuffer fb = m_CurrentOffscreenFB;
        auto* queue = m_VulkanDevice ? &m_VulkanDevice->GetDeferredDestroy() : nullptr;
        if (queue) {
            queue->Enqueue([dev, fb]() {
                vkDestroyFramebuffer(dev, fb, nullptr);
            });
        }
        m_CurrentOffscreenFB = VK_NULL_HANDLE;
    }
}

} // namespace he::rhi
