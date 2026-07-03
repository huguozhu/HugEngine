// ============================================================
// VulkanCommandList.cpp — Vulkan 命令缓冲实现
// 负责命令录制：Begin/End、RenderPass、Draw/Dispatch、Barrier、Submit
// ============================================================
#include "RHI/RHI.h"
#include "RHI/SwapChain.h"
#include "RHI/CommandList.h"
#include "RHI/Buffer.h"
#include "RHI/Shader.h"
#include "Core/Log.h"
#include "Core/Assert.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <algorithm>
#include <vector>
#include <cstring>

// VulkanSwapChain/VulkanCommandList 等类型的完整定义
#include "VulkanInternal.h"

namespace he::rhi {

// ============================================================
// ResourceState → VkImageLayout 映射（支持位组合，写状态优先）
// ============================================================
static VkImageLayout ToVkImageLayout(ResourceState state) {
    u32 s = u32(state);
    if (s & u32(ResourceState::DepthStencilWrite)) return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if (s & u32(ResourceState::DepthStencilRead))  return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    if (s & u32(ResourceState::RenderTarget))       return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (s & u32(ResourceState::ShaderResource))     return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (s & u32(ResourceState::Present))            return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    if (s & u32(ResourceState::UnorderedAccess))    return VK_IMAGE_LAYOUT_GENERAL;
    if (s & u32(ResourceState::CopySrc))            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (s & u32(ResourceState::CopyDst))            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

// PipelineStage → VkPipelineStageFlags 映射
static VkPipelineStageFlags ToVkPipelineStageFlags(PipelineStage stage) {
    VkPipelineStageFlags flags = 0;
    u32 s = u32(stage);
    if (s & u32(PipelineStage::ColorAttachmentOutput)) flags |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (s & u32(PipelineStage::FragmentShader))        flags |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    if (s & u32(PipelineStage::ComputeShader))         flags |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    if (s & u32(PipelineStage::Transfer))              flags |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (s & u32(PipelineStage::EarlyFragmentTests))    flags |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    if (s & u32(PipelineStage::LateFragmentTests))     flags |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    if (s & u32(PipelineStage::VertexShader))          flags |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    return flags ? flags : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
}

// ResourceState → VkAccessFlags 映射
static VkAccessFlags ToVkAccessFlags(ResourceState state) {
    VkAccessFlags flags = 0;
    u32 s = u32(state);
    if (s & u32(ResourceState::DepthStencilWrite)) flags |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    if (s & u32(ResourceState::DepthStencilRead))  flags |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    if (s & u32(ResourceState::ShaderResource))     flags |= VK_ACCESS_SHADER_READ_BIT;
    if (s & u32(ResourceState::RenderTarget))       flags |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (s & u32(ResourceState::UnorderedAccess))    flags |= VK_ACCESS_SHADER_WRITE_BIT;
    if (s & u32(ResourceState::CopySrc))            flags |= VK_ACCESS_TRANSFER_READ_BIT;
    if (s & u32(ResourceState::CopyDst))            flags |= VK_ACCESS_TRANSFER_WRITE_BIT;
    return flags ? flags : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
}

// ============================================================
// VulkanCommandList 构造/析构
// ============================================================

VulkanCommandList::VulkanCommandList(VkDevice device, VkQueue queue, u32 queueFamily,
                                     VulkanDevice* vulkanDevice)
    : m_Device(device), m_Queue(queue), m_QueueFamily(queueFamily)
    , m_VulkanDevice(vulkanDevice)
{
    // 创建三缓冲命令池 + 命令缓冲 + 栅栏（Phase 1 多线程渲染）
    for (u32 i = 0; i < kMaxFramesInFlight; ++i) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = m_QueueFamily;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_CmdPools[i]);

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_CmdPools[i];
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        vkAllocateCommandBuffers(m_Device, &allocInfo, &m_CmdBuffers[i]);

        // 创建时设为已触发状态，首次 Begin() 不会阻塞
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(m_Device, &fenceInfo, nullptr, &m_Fences[i]);
    }
}

// Phase 2: 辅助命令缓冲构造（预分配 kMaxSecondaryCBs 个 sec CB，避免复用冲突）
VulkanCommandList::VulkanCommandList(VkDevice device, u32 queueFamily,
                                     VulkanDevice* vulkanDevice)
    : m_Device(device), m_QueueFamily(queueFamily), m_VulkanDevice(vulkanDevice)
{
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = m_QueueFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_SecondaryPool);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_SecondaryPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    allocInfo.commandBufferCount = kMaxSecondaryCBs;
    vkAllocateCommandBuffers(m_Device, &allocInfo, m_SecCmdBuffers);
    m_SecSlot = 0;
}

VulkanCommandList::~VulkanCommandList() {
    vkDeviceWaitIdle(m_Device);
    for (auto& fb : m_Framebuffers) vkDestroyFramebuffer(m_Device, fb, nullptr);
    if (m_CurrentOffscreenFB) { vkDestroyFramebuffer(m_Device, m_CurrentOffscreenFB, nullptr); }
    for (u32 i = 0; i < kMaxFramesInFlight; ++i) {
        for (VkFramebuffer fb : m_PendingFBs[i]) { vkDestroyFramebuffer(m_Device, fb, nullptr); }
        m_PendingFBs[i].clear();
    }
    if (m_LoadRenderPass) { vkDestroyRenderPass(m_Device, m_LoadRenderPass, nullptr); }
    if (m_SecondaryPool) {
        vkDestroyCommandPool(m_Device, m_SecondaryPool, nullptr);
    } else {
        for (u32 i = 0; i < kMaxFramesInFlight; ++i) {
            if (m_Fences[i])    vkDestroyFence(m_Device, m_Fences[i], nullptr);
            if (m_CmdPools[i])  vkDestroyCommandPool(m_Device, m_CmdPools[i], nullptr);
        }
    }
}

// ============================================================
// 辅助命令缓冲（Secondary CB）
// ============================================================

void VulkanCommandList::BeginSecondary(IRHIPipelineState* pso) {
    // 模循环复用 Sec CB：kMaxSecondaryCBs == kMaxFramesInFlight，
    // 每个 CB 在 kMaxFramesInFlight 帧后复用，此时主 CB 栅栏已确保 GPU 完成。
    u32 idx = m_SecSlot % kMaxSecondaryCBs;

    auto* vkPSO = static_cast<VulkanPipelineState*>(pso);
    VkCommandBufferInheritanceInfo inheritInfo{};
    inheritInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    inheritInfo.renderPass = vkPSO->GetRenderPass();
    inheritInfo.subpass = 0;
    inheritInfo.framebuffer = VK_NULL_HANDLE;

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT
                    | VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    beginInfo.pInheritanceInfo = &inheritInfo;

    // vkBeginCommandBuffer 对已完成 CB 隐式重置（等价于 vkResetCommandBuffer）
    vkBeginCommandBuffer(m_SecCmdBuffers[idx], &beginInfo);
    // 别名给 SetPipeline/Draw 等 Vulkan 调用共用（m_FrameIndex=0 for secondary）
    m_CmdBuffers[m_FrameIndex] = m_SecCmdBuffers[idx];
    // 记录 PSO 状态并绑定额外管线（BindDescriptorSet 需要 pipeline layout）
    m_CurrentPipeline   = vkPSO->GetPipeline();
    m_CurrentLayout     = vkPSO->GetPipelineLayout();
    m_CurrentRenderPass = vkPSO->GetRenderPass();
    // 在 sec CB 中绑定额外管线（sec CB 不会继承 primary 的管线状态）
    vkCmdBindPipeline(m_SecCmdBuffers[idx], VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_CurrentPipeline);
    m_SecActive = idx;
    m_IsRecording = true;
}

void VulkanCommandList::ExecuteSecondary(IRHICommandList* secondary) {
    auto* vkSec = static_cast<VulkanCommandList*>(secondary);
    // 执行 sec CL 当前活跃的 sec CB（而非已被覆盖的旧 CB）
    vkCmdExecuteCommands(m_CmdBuffers[m_FrameIndex], 1,
                         &vkSec->m_SecCmdBuffers[vkSec->m_SecActive]);
}

// ============================================================
// Begin / End
// ============================================================

void VulkanCommandList::Begin() {
    // 等待当前帧槽位的 GPU 栅栏（非阻塞：仅等待该槽位的历史提交）
    vkWaitForFences(m_Device, 1, &m_Fences[m_FrameIndex], VK_TRUE, UINT64_MAX);

    // 安全销毁待处理 FB（fence 已等待，GPU 保证完成）
    for (VkFramebuffer fb : m_PendingFBs[m_FrameIndex]) { vkDestroyFramebuffer(m_Device, fb, nullptr); }
    m_PendingFBs[m_FrameIndex].clear();
    // 安全销毁旧 swapchain framebuffer 并标记重建
    if (m_FramebuffersNeedRebuild) {
        for (VkFramebuffer fb : m_Framebuffers) { vkDestroyFramebuffer(m_Device, fb, nullptr); }
        m_Framebuffers.clear();
    }

    // 重置该槽位的命令池
    vkResetCommandPool(m_Device, m_CmdPools[m_FrameIndex], 0);

    // 开始录制当前帧的命令缓冲
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(m_CmdBuffers[m_FrameIndex], &beginInfo);
    m_IsRecording = true;
}

void VulkanCommandList::End() {
    if (m_SecondaryPool) {
        // 辅助命令缓冲：结束当前槽位的 sec CB，槽位递增（模循环复用）
        vkEndCommandBuffer(m_SecCmdBuffers[m_SecActive]);
        ++m_SecSlot;
    } else {
        vkEndCommandBuffer(m_CmdBuffers[m_FrameIndex]);
    }
    m_IsRecording = false;
}

// ============================================================
// RenderPass（SwapChain 渲染目标）
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
            att[0].initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;  // 匹配 PSO RP 的 finalLayout（BGRA8_UNORM → Present）
            att[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            att[1].format = VK_FORMAT_D32_SFLOAT;
            att[1].samples = VK_SAMPLE_COUNT_1_BIT;
            att[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            att[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;  // 匹配所有 PSO RP 的 finalLayout
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
            dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

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
            // 旧 FB 已在 Begin() 中销毁，这里只清空句柄
            m_Framebuffers.clear();
        }
        m_FramebuffersNeedRebuild = false;
        u32 count = static_cast<u32>(m_SwapchainViews.size());
        m_Framebuffers.resize(count);
        for (u32 i = 0; i < count; ++i) {
            VkImageView attachments[2] = { m_SwapchainViews[i] };
            u32 attachmentCount = 1;

            // 如果 SwapChain 有深度纹理，作为第二个附件
            if (m_pSwapChain && m_pSwapChain->GetDepthImageView()) {
                attachments[1] = m_pSwapChain->GetDepthImageView();
                attachmentCount = 2;
            }

            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass      = rp;  // 使用最终选择的 RP（而非 m_CurrentRenderPass）
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
    // 顶点/索引缓冲在 Draw/DrawIndexed 时按需绑定，以支持正确的 binding 参数
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
    // 验证：至少需要一个附件
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

    // 构建附件列表
    VkImageView attachments[2] = {};
    u32 attachmentCount = 0;
    if (colorView) attachments[attachmentCount++] = colorView;
    if (depthView) attachments[attachmentCount++] = depthView;

    // 创建离屏 Framebuffer（每帧新建，旧的在 Begin() 中通过 fence 等待后安全销毁）
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
    m_CurrentOffscreenFB = offscreenFB;  // 记录当前 FB 以便延迟销毁

    // 清除值
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
        vkClearValues[clearCount].depthStencil.depth =
            clear ? clear->depth : 1.0f;
        vkClearValues[clearCount].depthStencil.stencil =
            clear ? clear->stencil : 0;
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

    // 在渲染通道内重新绑定管线（Vulkan 要求）
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

    // 构建附件列表：颜色在前，深度在后；支持最多 4 个颜色附件 + 1 个深度附件（共 5 个）
    VkImageView attachments[5] = {};
    u32 attachmentCount = 0;
    for (u32 i = 0; i < colorCount && attachmentCount < 5; ++i)
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

    // 清除值（最多 4 个颜色 + 1 个深度，共 5 个）
    VkClearValue vkClearValues[5]{};
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

    // FB 不能立即销毁 — CB 尚未提交。加入延迟销毁队列，
    // 在 3 帧后 Begin() 的 fence 等待后安全销毁。
    if (m_CurrentOffscreenFB) {
        m_PendingFBs[m_FrameIndex].push_back(m_CurrentOffscreenFB);
        m_CurrentOffscreenFB = VK_NULL_HANDLE;
    }
}

// ============================================================
// 状态设置
// ============================================================

void VulkanCommandList::SetViewport(const Viewport& vp) {
    VkViewport vkViewport{};
    vkViewport.x = vp.x;
    vkViewport.y = vp.y;
    vkViewport.width = vp.width;
    vkViewport.height = vp.height;
    vkViewport.minDepth = vp.minDepth;
    vkViewport.maxDepth = vp.maxDepth;
    vkCmdSetViewport(m_CmdBuffers[m_FrameIndex], 0, 1, &vkViewport);
}

void VulkanCommandList::SetScissor(const ScissorRect& sc) {
    VkRect2D vkScissor{};
    vkScissor.offset = {sc.x, sc.y};
    vkScissor.extent = {sc.width, sc.height};
    vkCmdSetScissor(m_CmdBuffers[m_FrameIndex], 0, 1, &vkScissor);
}

void VulkanCommandList::SetSwapChain(IRHISwapChain* swapchain) {
    // 保存 Vulkan SwapChain 指针，后续 BeginRenderPass/Submit 自动使用
    m_pSwapChain = static_cast<VulkanSwapChain*>(swapchain);

    // 预创建 Framebuffer 用的 ImageView 列表
    u32 count = 3;
    m_SwapchainViews.resize(count);
    for (u32 i = 0; i < count; ++i)
        m_SwapchainViews[i] = m_pSwapChain->GetImageView(i);
    m_SwapchainExtent = {m_pSwapChain->GetWidth(), m_pSwapChain->GetHeight()};
    m_FramebuffersNeedRebuild = true;  // 标记重建，旧 FB 在 Begin() 中安全销毁
}

void VulkanCommandList::SetPipeline(IRHIPipelineState* pso) {
    auto* vkPso = static_cast<VulkanPipelineState*>(pso);
    m_CurrentPipeline   = vkPso->GetPipeline();
    m_CurrentLayout     = vkPso->GetPipelineLayout();
    m_CurrentRenderPass = vkPso->GetRenderPass();
    m_CurrentBindPoint  = vkPso->GetBindPoint();

    // 仅当命令缓冲处于录制状态时绑定管线
    if (m_IsRecording) {
        vkCmdBindPipeline(m_CmdBuffers[m_FrameIndex], m_CurrentBindPoint,
                         m_CurrentPipeline);
    }

    // Render pass 变化时标记 framebuffer 需重建
    m_FramebuffersNeedRebuild = true;
}

void VulkanCommandList::SetVertexBuffer(IRHIBuffer* buffer, u32 binding) {
    auto* vkBuf = static_cast<VulkanBuffer*>(buffer);
    m_CurrentVB = vkBuf->GetHandle();
    m_VBBinding = binding;
}

void VulkanCommandList::SetIndexBuffer(IRHIBuffer* buffer, u32 offset) {
    auto* vkBuf = static_cast<VulkanBuffer*>(buffer);
    m_CurrentIB = vkBuf->GetHandle();
    m_IBOffset  = offset;
    // 根据缓冲区大小推断索引类型（4 字节 = UINT32，2 字节 = UINT16）
    m_CurrentIndexType = (vkBuf->GetSize() >= 4) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
}

// ============================================================
// Draw / Dispatch
// ============================================================

void VulkanCommandList::Draw(u32 vertexCount, u32 instanceCount,
                              u32 firstVertex, u32 firstInstance) {
    // 绑定当前顶点缓冲（使用记录下的 binding 索引）
    if (m_CurrentVB) {
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(m_CmdBuffers[m_FrameIndex], m_VBBinding, 1, &m_CurrentVB, &offset);
    }
    vkCmdDraw(m_CmdBuffers[m_FrameIndex], vertexCount, instanceCount, firstVertex, firstInstance);
}

void VulkanCommandList::DrawIndexed(u32 indexCount, u32 instanceCount,
                                     u32 firstIndex, i32 vertexOffset, u32 firstInstance) {
    // 绑定索引缓冲
    if (m_CurrentIB)
        vkCmdBindIndexBuffer(m_CmdBuffers[m_FrameIndex], m_CurrentIB, m_IBOffset, m_CurrentIndexType);
    // 绑定当前顶点缓冲
    if (m_CurrentVB) {
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(m_CmdBuffers[m_FrameIndex], m_VBBinding, 1, &m_CurrentVB, &offset);
    }
    vkCmdDrawIndexed(m_CmdBuffers[m_FrameIndex], indexCount, instanceCount,
                     firstIndex, vertexOffset, firstInstance);
}

void VulkanCommandList::SetPushConstants(u32 offset, u32 size, const void* data) {
    if (m_CurrentLayout) {
        VkShaderStageFlags stage = (m_CurrentBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE)
            ? VK_SHADER_STAGE_COMPUTE_BIT
            : (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
        vkCmdPushConstants(m_CmdBuffers[m_FrameIndex], m_CurrentLayout,
                          stage, offset, size, data);
    }
}

void VulkanCommandList::Dispatch(u32 groupCountX, u32 groupCountY, u32 groupCountZ) {
    vkCmdDispatch(m_CmdBuffers[m_FrameIndex], groupCountX, groupCountY, groupCountZ);
}

void VulkanCommandList::DispatchIndirect(IRHIBuffer* buffer, u64 offset) {
    auto* vkBuf = static_cast<VulkanBuffer*>(buffer);
    vkCmdDispatchIndirect(m_CmdBuffers[m_FrameIndex], vkBuf->GetHandle(), offset);
}

void VulkanCommandList::BindDescriptorSet(u32 setIndex, DescriptorSetHandle setHandle) {
    if (!m_VulkanDevice) return;
    VkDescriptorSet ds = m_VulkanDevice->ResolveDescriptorSet(setHandle);
    if (ds == VK_NULL_HANDLE) return;
    vkCmdBindDescriptorSets(m_CmdBuffers[m_FrameIndex], VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_CurrentLayout, setIndex, 1, &ds, 0, nullptr);
}

void VulkanCommandList::CopyBuffer(IRHIBuffer* src, IRHIBuffer* dst,
                                    u64 size, u64 srcOffset, u64 dstOffset) {
    auto* vkSrc = static_cast<VulkanBuffer*>(src);
    auto* vkDst = static_cast<VulkanBuffer*>(dst);

    VkBufferCopy region{};
    region.srcOffset = srcOffset;
    region.dstOffset = dstOffset;
    region.size      = size;

    vkCmdCopyBuffer(m_CmdBuffers[m_FrameIndex], vkSrc->GetHandle(), vkDst->GetHandle(), 1, &region);
}

// ============================================================
// Pipeline Barrier
// ============================================================

void VulkanCommandList::PipelineBarrier(
    PipelineStage srcStage, PipelineStage dstStage,
    ResourceState srcState, ResourceState dstState)
{
    VkMemoryBarrier memoryBarrier{};
    memoryBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    memoryBarrier.srcAccessMask = ToVkAccessFlags(srcState);
    memoryBarrier.dstAccessMask = ToVkAccessFlags(dstState);

    vkCmdPipelineBarrier(m_CmdBuffers[m_FrameIndex],
        ToVkPipelineStageFlags(srcStage), ToVkPipelineStageFlags(dstStage),
        0, 1, &memoryBarrier, 0, nullptr, 0, nullptr);
}

void VulkanCommandList::PipelineBarrier(
    PipelineStage srcStage, PipelineStage dstStage,
    ResourceState srcState, ResourceState dstState,
    IRHITexture* texture)
{
    if (!texture) return;
    auto* vkTex = static_cast<VulkanTexture*>(texture);

    VkImageMemoryBarrier imageBarrier{};
    imageBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imageBarrier.srcAccessMask       = ToVkAccessFlags(srcState);
    imageBarrier.dstAccessMask       = ToVkAccessFlags(dstState);
    imageBarrier.oldLayout           = ToVkImageLayout(srcState);
    imageBarrier.newLayout           = ToVkImageLayout(dstState);
    imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.image = vkTex->GetImage();

    // 根据纹理格式选择 aspect mask（深度/模板 vs 颜色）
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    Format fmt = vkTex->GetFormat();
    if (fmt == Format::D32_FLOAT || fmt == Format::D24_UNORM_S8_UINT || fmt == Format::D16_UNORM)
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    imageBarrier.subresourceRange = {
        aspect,
        0, vkTex->GetMipLevels(),
        0, vkTex->GetArrayLayers()   // Cubemap=6, 普通纹理=1
    };

    vkCmdPipelineBarrier(m_CmdBuffers[m_FrameIndex],
        ToVkPipelineStageFlags(srcStage), ToVkPipelineStageFlags(dstStage),
        0, 0, nullptr, 0, nullptr, 1, &imageBarrier);
}

// ============================================================
// Submit（提交命令缓冲到 GPU 队列）
// ============================================================

void VulkanCommandList::Submit() {
    // 等待 SwapChain 图像可用
    VkSemaphore waitSem   = VK_NULL_HANDLE;
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (m_pSwapChain) waitSem = m_pSwapChain->GetImageAcquiredSemaphore();

    // 渲染完成后通知 Present
    VkSemaphore signalSem = VK_NULL_HANDLE;
    if (m_pSwapChain) signalSem = m_pSwapChain->GetRenderCompleteSemaphore();

    VkCommandBuffer cb = m_CmdBuffers[m_FrameIndex];

    VkSubmitInfo submitInfo{};
    submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount   = waitSem ? 1u : 0u;
    submitInfo.pWaitSemaphores      = &waitSem;
    submitInfo.pWaitDstStageMask    = &waitStage;
    submitInfo.commandBufferCount   = 1;
    submitInfo.pCommandBuffers      = &cb;
    submitInfo.signalSemaphoreCount = signalSem ? 1u : 0u;
    submitInfo.pSignalSemaphores    = &signalSem;

    // 重置当前帧栅栏并提交（GPU 执行完成后触发栅栏）
    vkResetFences(m_Device, 1, &m_Fences[m_FrameIndex]);
    vkQueueSubmit(m_Queue, 1, &submitInfo, m_Fences[m_FrameIndex]);

    // 非阻塞：不等待 GPU 完成，下一帧 Begin() 中等待该槽位的历史栅栏
    // CPU 可提前 1-2 帧开始录制
    m_FrameIndex = (m_FrameIndex + 1) % kMaxFramesInFlight;
}

} // namespace he::rhi
