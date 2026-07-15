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
#include "VulkanQueryPool.h"

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
    if (s & u32(PipelineStage::VertexShader))             flags |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    if (s & u32(PipelineStage::RayTracingShader))          flags |= VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
    if (s & u32(PipelineStage::AccelerationStructureBuild)) flags |= VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
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
    if (m_DummyDepthView)   { vkDestroyImageView(m_Device, m_DummyDepthView, nullptr); }
    if (m_DummyDepthImage)  { vkDestroyImage(m_Device, m_DummyDepthImage, nullptr); }
    if (m_DummyDepthMemory) { vkFreeMemory(m_Device, m_DummyDepthMemory, nullptr); }
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

    // 构建附件列表（仅使用调用方提供的附件）
    VkImageView attachments[2] = {};
    u32 attachmentCount = 0;
    if (colorView) attachments[attachmentCount++] = colorView;
    if (depthView) attachments[attachmentCount++] = depthView;

    // 创建离屏 Framebuffer
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
    VkImageView attachments[8] = {};
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
    VkClearValue vkClearValues[8]{};
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

void VulkanCommandList::DrawIndexedIndirect(rhi::IRHIBuffer* buffer, u64 offset,
                                             u32 drawCount, u32 stride) {
    auto* vkBuf = static_cast<VulkanBuffer*>(buffer);
    vkCmdDrawIndexedIndirect(m_CmdBuffers[m_FrameIndex], vkBuf->GetHandle(),
                              (VkDeviceSize)offset, drawCount, stride);
}

// ============================================================
// ExecuteGeneratedCommands — DGC 执行入口
// GPU 根据 IndirectCommandsLayout 从间接缓冲读取参数并生成绘制命令
// ============================================================
void VulkanCommandList::ExecuteGeneratedCommands(const DGCExecuteDesc& desc) {
    if (!m_VulkanDevice || !m_VulkanDevice->SupportsDGC()) {
        HE_CORE_WARN("ExecuteGeneratedCommands: 设备不支持 DGC");
        return;
    }

    VkCommandBuffer cb = m_CmdBuffers[m_FrameIndex];
    const auto& dgcFuncs = m_VulkanDevice->GetDGCFuncs();

    // 从 DGCExecuteDesc 还原 Vulkan 句柄（非调度句柄需 reinterpret_cast）
    VkIndirectCommandsLayoutEXT layout =
        reinterpret_cast<VkIndirectCommandsLayoutEXT>(desc.indirectCommandsLayout);
    VkIndirectExecutionSetEXT executionSet =
        reinterpret_cast<VkIndirectExecutionSetEXT>(desc.indirectExecutionSet);

    // 构建 DGC 执行信息
    VkGeneratedCommandsInfoEXT genInfo{};
    genInfo.sType              = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT;
    genInfo.shaderStages       = VK_SHADER_STAGE_VERTEX_BIT
                                 | VK_SHADER_STAGE_FRAGMENT_BIT;
    genInfo.indirectExecutionSet   = executionSet;
    genInfo.indirectCommandsLayout = layout;
    genInfo.indirectAddress        = desc.sequencesBufferAddr;
    genInfo.indirectAddressSize    = desc.maxSequenceCount * sizeof(u32) * 5;  // VkDrawIndexedIndirectCommand
    genInfo.preprocessAddress      = desc.preprocessBufferAddr;
    genInfo.preprocessSize         = desc.preprocessBufferSize;
    genInfo.maxSequenceCount       = desc.maxSequenceCount;
    genInfo.sequenceCountAddress   = desc.sequenceCountAddr;
    genInfo.maxDrawCount           = desc.maxDrawCount;

    // 执行 DGC（isPreprocessed = VK_FALSE：由驱动内部完成预处理）
    dgcFuncs.vkCmdExecuteGeneratedCommandsEXT(
        cb, VK_FALSE, &genInfo);
}

void VulkanCommandList::SetPushConstants(u32 offset, u32 size, const void* data) {
    // 根据当前管线绑定点选择正确的 shader stage 标志
    VkShaderStageFlags stage;
    if (m_CurrentBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
        stage = VK_SHADER_STAGE_COMPUTE_BIT;
    } else if (m_CurrentBindPoint == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR) {
        // RT 管线：Push Constant 需覆盖所有 RT shader stage
        stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR
              | VK_SHADER_STAGE_MISS_BIT_KHR
              | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
              | VK_SHADER_STAGE_ANY_HIT_BIT_KHR
              | VK_SHADER_STAGE_CALLABLE_BIT_KHR;
    } else {
        stage = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    vkCmdPushConstants(m_CmdBuffers[m_FrameIndex], m_CurrentLayout,
                      stage, offset, size, data);
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
    // 使用 m_CurrentBindPoint（SetPipeline 已根据 Compute/Graphics PSO 设置正确的 bind point）
    vkCmdBindDescriptorSets(m_CmdBuffers[m_FrameIndex], m_CurrentBindPoint,
                            m_CurrentLayout, setIndex, 1, &ds, 0, nullptr);
}

// ── GPU Timestamp Query ──
void VulkanCommandList::WriteTimestamp(IRHIQueryPool* pool, u32 queryIndex) {
    auto* vkPool = static_cast<VulkanQueryPool*>(pool);
    vkCmdWriteTimestamp(m_CmdBuffers[m_FrameIndex],
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, vkPool->GetHandle(), queryIndex);
}

void VulkanCommandList::ResetQueryPool(IRHIQueryPool* pool) {
    auto* vkPool = static_cast<VulkanQueryPool*>(pool);
    vkCmdResetQueryPool(m_CmdBuffers[m_FrameIndex], vkPool->GetHandle(), 0, vkPool->GetQueryCount());
}

void VulkanCommandList::GetQueryResults(IRHIQueryPool* pool, u32 first, u32 count, u64* data) {
    auto* vkPool = static_cast<VulkanQueryPool*>(pool);
    vkGetQueryPoolResults(m_Device, vkPool->GetHandle(), first, count,
        sizeof(u64) * count, data, sizeof(u64),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
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

void VulkanCommandList::CopyTextureToTexture(IRHITexture* src, IRHITexture* dst) {
    auto* vkSrc = static_cast<VulkanTexture*>(src);
    auto* vkDst = static_cast<VulkanTexture*>(dst);
    u32 w = vkSrc->GetWidth(), h = vkSrc->GetHeight();

    // 源: ShaderRead → TransferSrc, 目标: Undefined/Present → TransferDst
    VkImageMemoryBarrier preBarriers[2]{};
    preBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    preBarriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    preBarriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    preBarriers[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    preBarriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    preBarriers[0].image = vkSrc->GetImage();
    preBarriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    preBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    preBarriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    preBarriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    preBarriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    preBarriers[1].image = vkDst->GetImage();
    preBarriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    vkCmdPipelineBarrier(m_CmdBuffers[m_FrameIndex],
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 2, preBarriers);

    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {w, h, 1};
    vkCmdCopyImage(m_CmdBuffers[m_FrameIndex],
        vkSrc->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        vkDst->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // 恢复: TransferSrc → ShaderRead, TransferDst → ShaderRead
    VkImageMemoryBarrier postBarriers[2]{};
    postBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    postBarriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    postBarriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    postBarriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    postBarriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    postBarriers[0].image = vkSrc->GetImage();
    postBarriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    postBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    postBarriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    postBarriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    postBarriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    postBarriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    postBarriers[1].image = vkDst->GetImage();
    postBarriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    vkCmdPipelineBarrier(m_CmdBuffers[m_FrameIndex],
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 2, postBarriers);
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
// 跨队列所有权转移（AsyncCompute Barrier）
// ============================================================

void VulkanCommandList::QueueOwnershipTransfer(
    IRHITexture* texture,
    QueueType srcQueue, QueueType dstQueue,
    ResourceState currentState, ResourceState newState)
{
    if (!texture || !m_VulkanDevice) return;
    auto* vkTex = static_cast<VulkanTexture*>(texture);

    u32 srcFamily = m_VulkanDevice->GetQueueFamily(srcQueue);
    u32 dstFamily = m_VulkanDevice->GetQueueFamily(dstQueue);

    VkImageMemoryBarrier imageBarrier{};
    imageBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imageBarrier.srcAccessMask       = ToVkAccessFlags(currentState);
    imageBarrier.dstAccessMask       = ToVkAccessFlags(newState);
    imageBarrier.oldLayout           = ToVkImageLayout(currentState);
    imageBarrier.newLayout           = ToVkImageLayout(newState);
    imageBarrier.srcQueueFamilyIndex = srcFamily;
    imageBarrier.dstQueueFamilyIndex = dstFamily;
    imageBarrier.image               = vkTex->GetImage();

    // 根据纹理格式选择 aspect mask
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    Format fmt = vkTex->GetFormat();
    if (fmt == Format::D32_FLOAT || fmt == Format::D24_UNORM_S8_UINT || fmt == Format::D16_UNORM)
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    imageBarrier.subresourceRange = {
        aspect,
        0, vkTex->GetMipLevels(),
        0, vkTex->GetArrayLayers()
    };

    vkCmdPipelineBarrier(m_CmdBuffers[m_FrameIndex],
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &imageBarrier);
}

void VulkanCommandList::QueueOwnershipTransfer(
    IRHIBuffer* buffer,
    QueueType srcQueue, QueueType dstQueue,
    ResourceState currentState, ResourceState newState)
{
    if (!buffer || !m_VulkanDevice) return;
    auto* vkBuf = static_cast<VulkanBuffer*>(buffer);

    u32 srcFamily = m_VulkanDevice->GetQueueFamily(srcQueue);
    u32 dstFamily = m_VulkanDevice->GetQueueFamily(dstQueue);

    VkBufferMemoryBarrier bufferBarrier{};
    bufferBarrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bufferBarrier.srcAccessMask       = ToVkAccessFlags(currentState);
    bufferBarrier.dstAccessMask       = ToVkAccessFlags(newState);
    bufferBarrier.srcQueueFamilyIndex = srcFamily;
    bufferBarrier.dstQueueFamilyIndex = dstFamily;
    bufferBarrier.buffer              = vkBuf->GetHandle();
    bufferBarrier.offset              = 0;
    bufferBarrier.size                = vkBuf->GetSize();

    vkCmdPipelineBarrier(m_CmdBuffers[m_FrameIndex],
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 1, &bufferBarrier, 0, nullptr);
}

void VulkanCommandList::ReleaseToQueue(IRHITexture* texture, QueueType dstQueue) {
    // 简化版: 释放资源到目标队列，保持 ShaderResource 状态
    // 调用者（RenderGraph）应在外部追踪精确状态，此处用安全默认值
    QueueOwnershipTransfer(texture, m_QueueType, dstQueue,
                           ResourceState::ShaderResource, ResourceState::ShaderResource);
}

void VulkanCommandList::AcquireFromQueue(IRHITexture* texture, QueueType srcQueue) {
    // 简化版: 从源队列获取资源，保持 ShaderResource 状态
    QueueOwnershipTransfer(texture, srcQueue, m_QueueType,
                           ResourceState::ShaderResource, ResourceState::ShaderResource);
}

// ============================================================
// Timeline Semaphore 集成（SetTimelineSignal / SetTimelineWait）
// ============================================================

void VulkanCommandList::SetTimelineSignal(RHIFenceHandle fence, u64 value) {
    if (!m_VulkanDevice || fence == kInvalidFence) return;
    // 通过 VulkanDevice 解析 FenceHandle → VkSemaphore
    // 直接访问内部 m_Fences（VulkanCommandList 和 VulkanDevice 是 friend/内部类）
    m_TimelineSignalSem = m_VulkanDevice->ResolveFenceSemaphore(fence);
    m_TimelineSignalVal = value;
}

void VulkanCommandList::SetTimelineWait(RHIFenceHandle fence, u64 value) {
    if (!m_VulkanDevice || fence == kInvalidFence) return;
    m_TimelineWaitSem = m_VulkanDevice->ResolveFenceSemaphore(fence);
    m_TimelineWaitVal = value;
}

// ============================================================
// Submit（提交命令缓冲到 GPU 队列）
// ============================================================

// ============================================================
// Mesh Shader 命令
// ============================================================

void VulkanCommandList::DrawMeshTasks(u32 groupCountX, u32 groupCountY, u32 groupCountZ) {
    if (!m_VulkanDevice->m_CmdDrawMeshTasks) {
        HE_CORE_WARN("DrawMeshTasks: 设备不支持 Mesh Shader");
        return;
    }
    VkCommandBuffer cb = m_CmdBuffers[m_FrameIndex];
    m_VulkanDevice->m_CmdDrawMeshTasks(cb, groupCountX, groupCountY, groupCountZ);
}

void VulkanCommandList::DrawMeshTasksIndirect(IRHIBuffer* buffer, u64 offset,
                                               u32 drawCount, u32 stride) {
    if (!m_VulkanDevice->m_CmdDrawMeshTasksIndirect) {
        HE_CORE_WARN("DrawMeshTasksIndirect: 设备不支持 Mesh Shader");
        return;
    }
    auto* vkBuf = static_cast<VulkanBuffer*>(buffer);
    VkCommandBuffer cb = m_CmdBuffers[m_FrameIndex];
    m_VulkanDevice->m_CmdDrawMeshTasksIndirect(cb, vkBuf->GetHandle(), offset, drawCount, stride);
}

// ============================================================
// Ray Tracing 命令 — 真实实现
// ============================================================

void VulkanCommandList::BuildBLAS(IRHIAccelerationStructure* blas, IRHIBuffer* scratchBuffer,
                                   const BLASBuildDesc& desc, bool update) {
    auto* vkBLAS  = static_cast<VulkanAccelerationStructure*>(blas);
    auto* vkScratch = static_cast<VulkanBuffer*>(scratchBuffer);
    auto& rt = m_VulkanDevice->GetRTDispatch();
    VkCommandBuffer cb = m_CmdBuffers[m_FrameIndex];

    // 1. 构建 VkAccelerationStructureGeometryKHR 数组
    std::vector<VkAccelerationStructureGeometryKHR> vkGeometries;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> rangeInfos;
    std::vector<u32> maxPrimCounts;

    for (auto& g : desc.geometries) {
        auto* vkVB = static_cast<VulkanBuffer*>(g.vertexBuffer);
        auto* vkIB = static_cast<VulkanBuffer*>(g.indexBuffer);
        auto* vkTF = g.transformBuffer ? static_cast<VulkanBuffer*>(g.transformBuffer) : nullptr;

        VkAccelerationStructureGeometryKHR vkGeo{};
        vkGeo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        vkGeo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        vkGeo.geometry.triangles.sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        vkGeo.geometry.triangles.vertexFormat  = ToVkFormat(g.vertexFormat);
        vkGeo.geometry.triangles.vertexData.deviceAddress = vkVB->GetDeviceAddress();
        vkGeo.geometry.triangles.vertexStride  = g.vertexStride;
        vkGeo.geometry.triangles.maxVertex     = g.maxVertex;
        vkGeo.geometry.triangles.indexType     = (g.indexFormat == Format::R32_UINT)
                                                  ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        vkGeo.geometry.triangles.indexData.deviceAddress  = vkIB ? vkIB->GetDeviceAddress() : 0;
        vkGeo.geometry.triangles.transformData.deviceAddress = vkTF ? vkTF->GetDeviceAddress() : 0;
        vkGeo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        vkGeometries.push_back(vkGeo);

        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount  = g.maxPrimitiveCount;
        range.primitiveOffset = 0;
        range.firstVertex     = 0;
        range.transformOffset = 0;
        rangeInfos.push_back(range);
        maxPrimCounts.push_back(g.maxPrimitiveCount);
    }

    // 2. 构建信息
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type                      = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags                     = ToVkBuildFlags(desc.flags);
    buildInfo.mode                      = update
        ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
        : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.srcAccelerationStructure  = update ? vkBLAS->GetHandle() : VK_NULL_HANDLE;
    buildInfo.dstAccelerationStructure  = vkBLAS->GetHandle();
    buildInfo.geometryCount             = static_cast<u32>(vkGeometries.size());
    buildInfo.pGeometries               = vkGeometries.data();
    buildInfo.scratchData.deviceAddress = vkScratch->GetDeviceAddress();

    // 3. 构建 BLAS
    VkAccelerationStructureBuildRangeInfoKHR* pRangeInfos = rangeInfos.data();
    rt.cmdBuildAS(cb, 1, &buildInfo, &pRangeInfos);
}

void VulkanCommandList::BuildTLAS(IRHIAccelerationStructure* tlas, IRHIBuffer* scratchBuffer,
                                   IRHIBuffer* instanceBuffer, u32 instanceCount, bool update) {
    auto* vkTLAS    = static_cast<VulkanAccelerationStructure*>(tlas);
    auto* vkScratch = static_cast<VulkanBuffer*>(scratchBuffer);
    auto* vkInstBuf = static_cast<VulkanBuffer*>(instanceBuffer);
    auto& rt = m_VulkanDevice->GetRTDispatch();
    VkCommandBuffer cb = m_CmdBuffers[m_FrameIndex];

    // 1. 实例几何描述
    VkAccelerationStructureGeometryKHR geoInfo{};
    geoInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geoInfo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geoInfo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geoInfo.geometry.instances.arrayOfPointers = VK_FALSE;
    geoInfo.geometry.instances.data.deviceAddress = vkInstBuf->GetDeviceAddress();

    // 2. 构建信息
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type                      = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags                     = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode                      = update
        ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
        : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.srcAccelerationStructure  = update ? vkTLAS->GetHandle() : VK_NULL_HANDLE;
    buildInfo.dstAccelerationStructure  = vkTLAS->GetHandle();
    buildInfo.geometryCount             = 1;
    buildInfo.pGeometries               = &geoInfo;
    buildInfo.scratchData.deviceAddress = vkScratch->GetDeviceAddress();

    // 3. 构建范围
    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount  = instanceCount;
    range.primitiveOffset = 0;
    range.firstVertex     = 0;
    range.transformOffset = 0;

    VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
    rt.cmdBuildAS(cb, 1, &buildInfo, &pRange);

    // 4. 内存屏障：确保 TLAS 构建完成后才能用于 TraceRays
    VkMemoryBarrier barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void VulkanCommandList::BindRTPipeline(IRHIRayTracingPipelineState* rtPSO) {
    auto* vkPSO = static_cast<VulkanRTPipelineState*>(rtPSO);
    VkCommandBuffer cb = m_CmdBuffers[m_FrameIndex];

    m_CurrentPipeline       = vkPSO->GetPipeline();
    m_CurrentLayout         = vkPSO->GetPipelineLayout();
    m_CurrentBindPoint      = VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, vkPSO->GetPipeline());
}

void VulkanCommandList::TraceRays(const SBTDesc& sbt, u32 width, u32 height, u32 depth) {
    auto& rt = m_VulkanDevice->GetRTDispatch();
    VkCommandBuffer cb = m_CmdBuffers[m_FrameIndex];

    // 解析 SBT 缓冲区 GPU 地址
    auto* vkBuf = static_cast<VulkanBuffer*>(sbt.buffer);
    u64 baseAddr = vkBuf->GetDeviceAddress();
    u64 bufSize  = vkBuf->GetSize();

    // 构建 VkStridedDeviceAddressRegionKHR（每个区域需要 deviceAddress + stride + size）
    // 区域大小从偏移差推断（假设紧密排列，末尾区域到缓冲末尾）
    auto makeRegion = [&](const SBTSlot& slot, u64 nextOffset) -> VkStridedDeviceAddressRegionKHR {
        VkStridedDeviceAddressRegionKHR region{};
        if (slot.stride == 0) return region;  // 空区域（deviceAddress=0, stride=0, size=0）

        region.deviceAddress = baseAddr + slot.handleOffset;
        region.stride        = slot.stride;
        u64 endOffset = (nextOffset > slot.handleOffset) ? nextOffset : bufSize;
        region.size = (endOffset > slot.handleOffset) ? (endOffset - slot.handleOffset) : slot.stride;
        return region;
    };

    // 收集有效区域偏移，排序后推断每个区域的终止位置
    u64 offsets[4] = {
        sbt.rayGen.stride   ? sbt.rayGen.handleOffset   : ~0ull,
        sbt.miss.stride     ? sbt.miss.handleOffset     : ~0ull,
        sbt.hit.stride      ? sbt.hit.handleOffset      : ~0ull,
        sbt.callable.stride ? sbt.callable.handleOffset : ~0ull,
    };
    std::sort(offsets, offsets + 4);

    auto getNext = [&](u64 myOffset) -> u64 {
        for (u64 o : offsets) if (o > myOffset && o != ~0ull) return o;
        return bufSize;
    };

    auto rayGenRegion   = makeRegion(sbt.rayGen,   getNext(sbt.rayGen.handleOffset));
    auto missRegion     = makeRegion(sbt.miss,     getNext(sbt.miss.handleOffset));
    auto hitRegion      = makeRegion(sbt.hit,      getNext(sbt.hit.handleOffset));
    auto callableRegion = makeRegion(sbt.callable, getNext(sbt.callable.handleOffset));

    rt.cmdTraceRays(cb,
        &rayGenRegion, &missRegion, &hitRegion, &callableRegion,
        width, height, depth);
}

void VulkanCommandList::Submit() {
    // 等待 SwapChain 图像可用
    VkSemaphore waitSem   = VK_NULL_HANDLE;
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (m_pSwapChain) waitSem = m_pSwapChain->GetImageAcquiredSemaphore();

    // 渲染完成后通知 Present
    VkSemaphore signalSem = VK_NULL_HANDLE;
    if (m_pSwapChain) signalSem = m_pSwapChain->GetRenderCompleteSemaphore();

    VkCommandBuffer cb = m_CmdBuffers[m_FrameIndex];

    // 构建二进制 + Timeline 信号量列表
    // Vulkan 允许在 pSignalSemaphores 中混合二进制和 Timeline 信号量
    VkSemaphore allSignalSems[2];
    u64        timelineSignalVals[2];
    u32        signalCount = 0;
    if (signalSem) allSignalSems[signalCount++] = signalSem;
    if (m_TimelineSignalSem) {
        allSignalSems[signalCount] = m_TimelineSignalSem;
        timelineSignalVals[signalCount] = m_TimelineSignalVal;
        signalCount++;
    }

    // 构建二进制 + Timeline 等待信号量列表
    VkSemaphore allWaitSems[2];
    u64        timelineWaitVals[2];
    VkPipelineStageFlags waitStages[2];
    u32        waitCount = 0;
    if (waitSem) {
        allWaitSems[waitCount] = waitSem;
        waitStages[waitCount] = waitStage;
        waitCount++;
    }
    if (m_TimelineWaitSem) {
        allWaitSems[waitCount] = m_TimelineWaitSem;
        waitStages[waitCount] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        timelineWaitVals[waitCount] = m_TimelineWaitVal;
        waitCount++;
    }

    // 构建 Timeline Semaphore Submit Info（pNext 链）
    // 注意：binary semaphore 不使用 timeline values，但 pWaitSemaphoreValues 必须
    // 为每个 wait semaphore 提供值。对于 binary semaphore，该值被忽略。
    VkTimelineSemaphoreSubmitInfo timelineInfo{};
    timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timelineInfo.waitSemaphoreValueCount   = waitCount;
    timelineInfo.pWaitSemaphoreValues      = timelineWaitVals;
    timelineInfo.signalSemaphoreValueCount = signalCount;
    timelineInfo.pSignalSemaphoreValues    = timelineSignalVals;

    VkSubmitInfo submitInfo{};
    submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pNext                = &timelineInfo;  // Timeline Semaphore 扩展
    submitInfo.waitSemaphoreCount   = waitCount;
    submitInfo.pWaitSemaphores      = allWaitSems;
    submitInfo.pWaitDstStageMask    = waitStages;
    submitInfo.commandBufferCount   = 1;
    submitInfo.pCommandBuffers      = &cb;
    submitInfo.signalSemaphoreCount = signalCount;
    submitInfo.pSignalSemaphores    = allSignalSems;

    // 重置当前帧栅栏并提交（GPU 执行完成后触发栅栏）
    vkResetFences(m_Device, 1, &m_Fences[m_FrameIndex]);
    vkQueueSubmit(m_Queue, 1, &submitInfo, m_Fences[m_FrameIndex]);

    // 清除 Timeline 信号量状态（下次 Submit 需重新设置）
    m_TimelineSignalSem = VK_NULL_HANDLE;
    m_TimelineSignalVal = 0;
    m_TimelineWaitSem   = VK_NULL_HANDLE;
    m_TimelineWaitVal   = 0;

    // 非阻塞：不等待 GPU 完成，下一帧 Begin() 中等待该槽位的历史栅栏
    // CPU 可提前 1-2 帧开始录制
    m_FrameIndex = (m_FrameIndex + 1) % kMaxFramesInFlight;
}

} // namespace he::rhi
