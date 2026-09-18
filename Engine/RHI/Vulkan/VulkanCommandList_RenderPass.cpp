// VulkanCommandList_RenderPass.cpp — BeginRenderPass / OffscreenPass 渲染通道管理
// 从 VulkanCommandList.cpp 拆分

#include "RHI/RHI.h"
#include "RHI/SwapChain.h"
#include "Core/Log.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "VulkanCommandList.h"
#include "VulkanDevice.h"
#include "RHI/TextureLayoutTracker.h"   // 布局追踪 + 视图→图像登记（render pass 边界维护）
#include "VulkanConverters.h"          // UsesPresentSrcFinalLayout（finalLayout 的唯一判定规则）

#include <cstdlib>

namespace he::rhi {

// ============================================================
// Framebuffer 创建/销毁追踪（诊断，默认关闭）
//
// 用途：校验层报 "command buffer ... was in an invalid state ... VkFramebuffer ...
// was destroyed" 时，需要知道**这些句柄是谁、何时**创建与销毁的，才能对账定位
// （06.GILab 启动阶段实测 110 次，其中一次列出 20 个句柄）。
// 打开方式：环境变量 HE_TRACE_FB=1（首次调用时读取，运行时零开销）。
// enqueueFrame / execFrame 两个帧号用来暴露"入队到真正销毁之间隔了几帧"。
// ============================================================
static void TraceFramebuffer(const char* action, VkFramebuffer fb,
                             u64 enqueueFrame, u64 execFrame) {
    static const bool s_Enabled = (std::getenv("HE_TRACE_FB") != nullptr);
    if (!s_Enabled) return;
    if (execFrame == UINT64_MAX) {
        HE_CORE_WARN("[FB] {:<20} handle={} frame={}", action, (void*)fb, enqueueFrame);
    } else {
        HE_CORE_WARN("[FB] {:<20} handle={} enqueueFrame={} execFrame={} delay={}",
                     action, (void*)fb, enqueueFrame, execFrame, execFrame - enqueueFrame);
    }
}

/// 当前帧号（无设备时返回 0）
static u64 CurrentFrameOf(VulkanDevice* dev) { return dev ? dev->GetCurrentFrame() : 0; }

// ============================================================
// render pass 边界的深度布局维护
//
// 引擎的 render pass 对深度附件声明：
//   initialLayout = ATTACHMENT（depthLoadOp == Load）或 UNDEFINED（Clear）
//   finalLayout   = READ_ONLY（"写完即可采样"，见 VulkanPipeline.cpp）
// 但 pass 自身的这些转变不经过 barrier，所以必须在这里同步给布局追踪器；
// 并在开始 pass 前把真实布局修正到 ATTACHMENT，否则以 Load 开始的 pass 会报
//   VUID-vkCmdBeginRenderPass-initialLayout-00900（实测 Skybox pass 每帧 1 次）。
// ============================================================

/// 深度相关的 ResourceState → VkImageLayout（只覆盖深度会用到的状态）
static VkImageLayout ToDepthLayout(u32 state) {
    using RS = rhi::ResourceState;
    if (state & u32(RS::DepthStencilWrite)) return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if (state & u32(RS::DepthStencilRead))  return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    if (state & u32(RS::ShaderResource))    return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
}

// ============================================================
// TrackColorAttachmentsAfterPass — 把颜色附件「pass 结束后的真实布局」写回追踪器
//
// 动机（TextureLayoutTracker.h 里写明的既有缺口：render pass 自身的 initial/final 布局转变
// 此前不记账）：RHI 建 RenderPass 时按 UsesPresentSrcFinalLayout(格式) 决定颜色附件的
// finalLayout（BGRA8/HDR10 ⇒ PRESENT_SRC，其余 ⇒ COLOR_ATTACHMENT）。若不在 pass 边界同步，
// 下一帧对同一张图发 barrier 时 oldLayout 会与实际不符 —— 实测：SDR 下 ToneMap 写的是 BGRA8
// 的 LDR 中间纹理（不是交换链），pass 结束后它停在 PRESENT_SRC，而追踪器还记着 RenderTarget，
// 于是 FXAA 的读 barrier 报 VUID-VkImageMemoryBarrier-oldLayout-01197（每帧 1 条）。
// ============================================================
static void TrackColorAttachmentsAfterPass(void* const* colorViews, u32 count) {
    if (!colorViews) return;
    for (u32 i = 0; i < count; ++i) {
        void* view = colorViews[i];
        if (!view) continue;
        rhi::ResourceState state = rhi::ResourceState::Present;   // 未登记视图 = 交换链图像视图
        u32 fmt = 0;
        if (rhi::QueryViewFormat(view, fmt) &&
            !rhi::UsesPresentSrcFinalLayout(static_cast<rhi::Format>(fmt))) {
            state = rhi::ResourceState::RenderTarget;
        }
        rhi::TrackTextureLayout(view, state);
    }
}

void VulkanCommandList::EnsureDepthAttachmentLayout(void* depthImageView) {
    if (!depthImageView) return;

    // 需要由视图反查底层图像（image barrier 只能作用于 VkImage）
    void* image = nullptr;
    u32   mips = 1, layers = 1;
    if (!QueryViewImage(depthImageView, image, mips, layers) || !image) return;

    // 从未记录过（该图还没参与过任何 barrier）→ 交给 render pass 的 UNDEFINED/首次使用语义
    rhi::ResourceState tracked;
    if (!QueryTrackedTextureLayout(depthImageView, tracked)) return;

    const VkImageLayout oldLayout = ToDepthLayout(u32(tracked));
    if (oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) return;   // 已经就位

    VkImageMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    // 保守的访问范围：来源可能是"被采样"或"被当作深度附件写"
    b.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    b.oldLayout           = oldLayout;
    b.newLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = static_cast<VkImage>(image);
    b.subresourceRange    = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, mips, 0, layers };

    vkCmdPipelineBarrier(m_CmdBuffers[m_FrameIndex],
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b);

    // 同步追踪器：此后该图处于 ATTACHMENT
    TrackTextureLayout(depthImageView, rhi::ResourceState::DepthStencilWrite);
}

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
            att[0].format = m_pSwapChain ? m_pSwapChain->GetVkFormat() : VK_FORMAT_B8G8R8A8_UNORM; // SwapChain 实际格式
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
        // 将旧 Framebuffer 延迟销毁（3 帧后 GPU 保证不再使用）
        // 不能立即销毁 — Vulkan 规范禁止在录制中销毁已绑定过的 FB
        if (!m_Framebuffers.empty()) {
            auto* queue = m_VulkanDevice ? &m_VulkanDevice->GetDeferredDestroy() : nullptr;
            for (VkFramebuffer fb : m_Framebuffers) {
                if (fb && queue) {
                    VkDevice dev = m_Device;
                    VulkanDevice* vd = m_VulkanDevice;
                    const u64 enq = CurrentFrameOf(vd);
                    queue->Enqueue([dev, fb, vd, enq]() {
                        TraceFramebuffer("destroy(swap-rebuild)", fb, enq, CurrentFrameOf(vd));
                        vkDestroyFramebuffer(dev, fb, nullptr);
                    });
                }
            }
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
            TraceFramebuffer("create(swapchain)", m_Framebuffers[i], CurrentFrameOf(m_VulkanDevice), UINT64_MAX);
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

    // ── LOAD 版 RenderPass 的附件布局确保 ──
    // 这个 RenderPass **固定**声明 颜色 initialLayout=PRESENT_SRC_KHR、深度 initialLayout=DEPTH_STENCIL_READ_ONLY_OPTIMAL
    // （它是为"保留 BackBuffer 内容 + 与 ToneMap PSO 的 RP 兼容"而定制的）。Vulkan 要求进入时附件**确实**处于
    // 声明的 initialLayout，而引擎此前没有做这件事：首帧两张图都还是 UNDEFINED；RT 直写 BackBuffer 的路径
    // （01.Triangle）会把它留在 COLOR_ATTACHMENT_OPTIMAL。校验层因此报
    //   "expects VkImage … to be in layout VK_IMAGE_LAYOUT_PRESENT_SRC_KHR … instead, current layout is …"
    // 这里按追踪器里的真实布局补一次转换（追踪器没有记录 = 首次使用，从 UNDEFINED 转换永远合法）。
    if (loadOp == LoadOp::Load && m_pSwapChain) {
        auto ensureLayout = [&](void* view, VkImage image, VkImageLayout wanted,
                                VkImageAspectFlags aspect, rhi::ResourceState wantedState) {
            if (!image || wanted == VK_IMAGE_LAYOUT_UNDEFINED) return;
            VkImageLayout old = VK_IMAGE_LAYOUT_UNDEFINED;
            rhi::ResourceState tracked;
            if (view && rhi::QueryTrackedTextureLayout(view, tracked)) {
                const u32 s = u32(tracked);
                if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT)             old = ToDepthLayout(s);
                else if (s & u32(rhi::ResourceState::Present))       old = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                else if (s & u32(rhi::ResourceState::RenderTarget))  old = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                else                                                 old = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            if (old == wanted) return;
            VkImageMemoryBarrier b{};
            b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask       = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            b.dstAccessMask       = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            b.oldLayout           = old;
            b.newLayout           = wanted;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image               = image;
            b.subresourceRange    = { aspect, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(m_CmdBuffers[m_FrameIndex],
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0, 0, nullptr, 0, nullptr, 1, &b);
            if (view) rhi::TrackTextureLayout(view, wantedState);
        };
        const u32 idx = m_CurrentImageIndex;
        ensureLayout(idx < m_SwapchainViews.size() ? m_SwapchainViews[idx] : nullptr,
                     m_pSwapChain->GetImage(idx), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_IMAGE_ASPECT_COLOR_BIT, rhi::ResourceState::Present);
        ensureLayout(m_pSwapChain->GetDepthImageView(), m_pSwapChain->GetDepthImage(),
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                     VK_IMAGE_ASPECT_DEPTH_BIT, rhi::ResourceState::DepthStencilRead);
    }

    vkCmdBeginRenderPass(m_CmdBuffers[m_FrameIndex], &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    // 交换链图像 pass 结束后停在 PRESENT_SRC（其 RenderPass 的 finalLayout）
    {
        void* bbView = (m_CurrentImageIndex < m_SwapchainViews.size())
                     ? m_SwapchainViews[m_CurrentImageIndex] : nullptr;
        TrackColorAttachmentsAfterPass(&bbView, 1);
    }

    // 绑定管线 — 仅当最后绑定的管线是图形管线时才绑定
    // （RT/Compute 管线的 m_CurrentBindPoint 不匹配 GRAPHICS，
    //   强行绑定会导致 Vulkan 验证错误 → 崩溃）
    if (m_CurrentPipeline && m_CurrentBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
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

    // 该 pass 会写入这两个附件：登记为"已写入"，供「采样了从未写入的纹理」检测使用（§9.2-S）
    MarkViewWritten(colorImageView);
    MarkViewWritten(depthImageView);

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
    TraceFramebuffer("create(offscreen)", offscreenFB, CurrentFrameOf(m_VulkanDevice), UINT64_MAX);
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

    // 开始 pass 前：把深度附件的真实布局修正到本 pass 期望的 ATTACHMENT
    EnsureDepthAttachmentLayout(depthImageView);

    // VK_SUBPASS_CONTENTS_INLINE_AND_SECONDARY_COMMAND_BUFFERS_KHR 要求设备启用
    // VK_KHR_maintenance7（nestedCommandBuffer）或 VK_EXT_nested_command_buffer；
    // 未启用时退回 INLINE，避免 VUID-vkCmdBeginRenderPass-contents-parameter / -09640。
    VkSubpassContents contents = (allowSecondary && m_VulkanDevice
                                  && m_VulkanDevice->SupportsMaintenance7())
        ? VK_SUBPASS_CONTENTS_INLINE_AND_SECONDARY_COMMAND_BUFFERS_KHR
        : VK_SUBPASS_CONTENTS_INLINE;
    vkCmdBeginRenderPass(m_CmdBuffers[m_FrameIndex], &rpBegin, contents);
    m_InOffscreenPass = true;

    // 颜色附件：按 RenderPass 的 finalLayout 记下「pass 结束后」的真实布局
    {
        void* cv = colorImageView;
        TrackColorAttachmentsAfterPass(&cv, 1);
    }

    // pass 期间深度是附件：同步追踪器，并记住深度视图供 EndOffscreenPass 收尾
    m_CurrentOffscreenDepthView = depthImageView;
    if (depthImageView) TrackTextureLayout(depthImageView, rhi::ResourceState::DepthStencilWrite);

    // 仅图形管线：RT/Compute 管线 bind point 不匹配 GRAPHICS
    if (m_CurrentPipeline != VK_NULL_HANDLE && m_CurrentBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
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

    // 构建附件列表：颜色在前，深度在后。上限是 kMaxColorAttachments（8）个颜色 + 1 个深度。
    // 【此前这里写死 7】颜色附件一律被截到 7 个，而 render pass 是按 PSO 的 colorAttachmentCount
    // 建的（可到 8）⇒ `vkCreateFramebuffer` 收到 8 个而 render pass 期望 9 个，
    // 校验层报 "attachmentCount 8 does not match 9"，随后驱动在 vkCmdBeginRenderPass 崩溃
    // （任务 31 加第 8 个 GBuffer MRT 时踩到）。数组必须按"颜色上限 + 1（深度）"开。
    VkImageView attachments[kMaxColorAttachments + 1] = {};
    u32 attachmentCount = 0;
    for (u32 i = 0; i < colorCount && attachmentCount < kMaxColorAttachments; ++i)
        attachments[attachmentCount++] = static_cast<VkImageView>(colorImageViews[i]);
    auto depthView = static_cast<VkImageView>(depthImageView);
    u32 depthIndex = attachmentCount;
    if (depthView) attachments[attachmentCount++] = depthView;

    // 同上：本 pass 会写入这些附件，登记为"已写入"
    for (u32 i = 0; i < colorCount && i < kMaxColorAttachments; ++i) MarkViewWritten(colorImageViews[i]);
    MarkViewWritten(depthImageView);

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
    TraceFramebuffer("create(offscreen)", offscreenFB, CurrentFrameOf(m_VulkanDevice), UINT64_MAX);
    m_CurrentOffscreenFB = offscreenFB;

    // 清除值（颜色数 + 1 个深度，与上面的附件数组同尺寸）
    VkClearValue vkClearValues[kMaxColorAttachments + 1]{};
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

    // 开始 pass 前：把深度附件的真实布局修正到本 pass 期望的 ATTACHMENT
    EnsureDepthAttachmentLayout(depthImageView);

    // VK_SUBPASS_CONTENTS_INLINE_AND_SECONDARY_COMMAND_BUFFERS_KHR 要求设备启用
    // VK_KHR_maintenance7（nestedCommandBuffer）或 VK_EXT_nested_command_buffer；
    // 未启用时退回 INLINE，避免 VUID-vkCmdBeginRenderPass-contents-parameter / -09640。
    VkSubpassContents contents = (allowSecondary && m_VulkanDevice
                                  && m_VulkanDevice->SupportsMaintenance7())
        ? VK_SUBPASS_CONTENTS_INLINE_AND_SECONDARY_COMMAND_BUFFERS_KHR
        : VK_SUBPASS_CONTENTS_INLINE;
    vkCmdBeginRenderPass(m_CmdBuffers[m_FrameIndex], &rpBegin, contents);
    m_InOffscreenPass = true;

    // 颜色附件：按 RenderPass 的 finalLayout 记下「pass 结束后」的真实布局
    TrackColorAttachmentsAfterPass(colorImageViews, colorCount);

    // pass 期间深度是附件：同步追踪器，并记住深度视图供 EndOffscreenPass 收尾
    m_CurrentOffscreenDepthView = depthImageView;
    if (depthImageView) TrackTextureLayout(depthImageView, rhi::ResourceState::DepthStencilWrite);

    // 仅图形管线：RT/Compute 管线 bind point 不匹配 GRAPHICS
    if (m_CurrentPipeline != VK_NULL_HANDLE && m_CurrentBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        vkCmdBindPipeline(m_CmdBuffers[m_FrameIndex], VK_PIPELINE_BIND_POINT_GRAPHICS, m_CurrentPipeline);
    }
}

void VulkanCommandList::EndOffscreenPass() {
    if (!m_InOffscreenPass) return;
    vkCmdEndRenderPass(m_CmdBuffers[m_FrameIndex]);
    m_InOffscreenPass = false;

    // pass 结束后深度停在 READ_ONLY（引擎 render pass 的 finalLayout 声明），
    // 这个转变不经过 barrier，必须在这里同步给追踪器，否则后续以 Load 开始的 pass
    // 会用错误的 oldLayout（见 EnsureDepthAttachmentLayout 的说明）
    if (m_CurrentOffscreenDepthView) {
        TrackTextureLayout(m_CurrentOffscreenDepthView, rhi::ResourceState::DepthStencilRead);
        m_CurrentOffscreenDepthView = nullptr;
    }

    // FB 不能立即销毁 — CB 尚未提交。
    // 通过 VulkanDevice 的延迟销毁队列统一管理，3 帧后安全销毁。
    if (m_CurrentOffscreenFB) {
        VkDevice dev = m_Device;
        VkFramebuffer fb = m_CurrentOffscreenFB;
        auto* queue = m_VulkanDevice ? &m_VulkanDevice->GetDeferredDestroy() : nullptr;
        if (queue) {
            VulkanDevice* vd = m_VulkanDevice;
            const u64 enq = CurrentFrameOf(vd);
            queue->Enqueue([dev, fb, vd, enq]() {
                TraceFramebuffer("destroy(offscreen)", fb, enq, CurrentFrameOf(vd));
                vkDestroyFramebuffer(dev, fb, nullptr);
            });
        }
        m_CurrentOffscreenFB = VK_NULL_HANDLE;
    }
}

} // namespace he::rhi
