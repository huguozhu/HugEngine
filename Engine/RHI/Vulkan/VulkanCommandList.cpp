// ============================================================
// VulkanCommandList.cpp — Vulkan 命令缓冲录制核心
//
// 拆分:
//   VulkanCommandList_RenderPass.cpp — BeginRenderPass / OffscreenPass
//   VulkanCommandList_Submit.cpp      — Submit / 跨队列同步 / Timeline
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

#include "VulkanCommandList.h"
#include "VulkanDevice.h"
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
    if (s & u32(PipelineStage::ColorAttachmentOutput))   flags |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (s & u32(PipelineStage::FragmentShader))          flags |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    if (s & u32(PipelineStage::ComputeShader))           flags |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    if (s & u32(PipelineStage::Transfer))                flags |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (s & u32(PipelineStage::EarlyFragmentTests))      flags |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    if (s & u32(PipelineStage::LateFragmentTests))       flags |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
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
// 构造/析构
// ============================================================

VulkanCommandList::VulkanCommandList(VkDevice device, VkQueue queue, u32 queueFamily,
                                     VulkanDevice* vulkanDevice)
    : m_Device(device), m_Queue(queue), m_QueueFamily(queueFamily)
    , m_VulkanDevice(vulkanDevice)
{
    // 创建三缓冲命令池 + 命令缓冲 + 栅栏
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

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(m_Device, &fenceInfo, nullptr, &m_Fences[i]);
    }
}

// 辅助命令缓冲构造（预分配 kMaxSecondaryCBs 个 sec CB，避免复用冲突）
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
    // 等待 GPU 完成所有工作，确保延迟销毁队列中的资源可以安全释放
    vkDeviceWaitIdle(m_Device);

    // 清空延迟销毁队列中的 Framebuffer（由 VulkanDevice 统一管理）
    if (m_VulkanDevice) {
        m_VulkanDevice->GetDeferredDestroy().FlushAll();
    }

    for (auto& fb : m_Framebuffers) vkDestroyFramebuffer(m_Device, fb, nullptr);
    if (m_CurrentOffscreenFB) { vkDestroyFramebuffer(m_Device, m_CurrentOffscreenFB, nullptr); }
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

    vkBeginCommandBuffer(m_SecCmdBuffers[idx], &beginInfo);
    m_CmdBuffers[m_FrameIndex] = m_SecCmdBuffers[idx];
    m_CurrentPipeline   = vkPSO->GetPipeline();
    m_CurrentLayout     = vkPSO->GetPipelineLayout();
    m_CurrentRenderPass = vkPSO->GetRenderPass();
    vkCmdBindPipeline(m_SecCmdBuffers[idx], VK_PIPELINE_BIND_POINT_GRAPHICS,
                      m_CurrentPipeline);
    m_SecActive = idx;
    m_IsRecording = true;
}

void VulkanCommandList::ExecuteSecondary(IRHICommandList* secondary) {
    auto* vkSec = static_cast<VulkanCommandList*>(secondary);
    vkCmdExecuteCommands(m_CmdBuffers[m_FrameIndex], 1,
                         &vkSec->m_SecCmdBuffers[vkSec->m_SecActive]);
}

// ============================================================
// Begin / End
// ============================================================

void VulkanCommandList::Begin() {
    // 等待当前帧槽位的 GPU 栅栏
    vkWaitForFences(m_Device, 1, &m_Fences[m_FrameIndex], VK_TRUE, UINT64_MAX);

    // 推进延迟销毁队列（fence 已等待，GPU 保证完成）。
    // AdvanceDeferredDestroy 内部有帧 ID 去重，多 CommandList 时只执行一次。
    if (m_VulkanDevice) {
        m_VulkanDevice->AdvanceFrame();
        m_VulkanDevice->AdvanceDeferredDestroy(m_VulkanDevice->GetCurrentFrame());
    }

    if (m_FramebuffersNeedRebuild) {
        for (VkFramebuffer fb : m_Framebuffers) { vkDestroyFramebuffer(m_Device, fb, nullptr); }
        m_Framebuffers.clear();
    }

    vkResetCommandPool(m_Device, m_CmdPools[m_FrameIndex], 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(m_CmdBuffers[m_FrameIndex], &beginInfo);
    m_IsRecording = true;
}

void VulkanCommandList::End() {
    if (m_SecondaryPool) {
        vkEndCommandBuffer(m_SecCmdBuffers[m_SecActive]);
        ++m_SecSlot;
    } else {
        vkEndCommandBuffer(m_CmdBuffers[m_FrameIndex]);
    }
    m_IsRecording = false;
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
    m_pSwapChain = static_cast<VulkanSwapChain*>(swapchain);

    u32 count = kSwapchainImageCount;  // Triple buffering 图像数
    m_SwapchainViews.resize(count);
    for (u32 i = 0; i < count; ++i)
        m_SwapchainViews[i] = m_pSwapChain->GetImageView(i);
    m_SwapchainExtent = {m_pSwapChain->GetWidth(), m_pSwapChain->GetHeight()};
    m_FramebuffersNeedRebuild = true;
}

void VulkanCommandList::SetPipeline(IRHIPipelineState* pso) {
    auto* vkPso = static_cast<VulkanPipelineState*>(pso);
    m_CurrentPipeline   = vkPso->GetPipeline();
    m_CurrentLayout     = vkPso->GetPipelineLayout();
    m_CurrentRenderPass = vkPso->GetRenderPass();
    m_CurrentBindPoint  = vkPso->GetBindPoint();

    if (m_IsRecording) {
        vkCmdBindPipeline(m_CmdBuffers[m_FrameIndex], m_CurrentBindPoint,
                         m_CurrentPipeline);
    }

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
    m_CurrentIndexType = (vkBuf->GetSize() >= 4) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
}

// ============================================================
// Draw / Dispatch
// ============================================================

void VulkanCommandList::Draw(u32 vertexCount, u32 instanceCount,
                              u32 firstVertex, u32 firstInstance) {
    if (m_CurrentVB) {
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(m_CmdBuffers[m_FrameIndex], m_VBBinding, 1, &m_CurrentVB, &offset);
    }
    vkCmdDraw(m_CmdBuffers[m_FrameIndex], vertexCount, instanceCount, firstVertex, firstInstance);
}

void VulkanCommandList::DrawIndexed(u32 indexCount, u32 instanceCount,
                                     u32 firstIndex, i32 vertexOffset, u32 firstInstance) {
    if (m_CurrentIB)
        vkCmdBindIndexBuffer(m_CmdBuffers[m_FrameIndex], m_CurrentIB, m_IBOffset, m_CurrentIndexType);
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
// ============================================================
void VulkanCommandList::ExecuteGeneratedCommands(const DGCExecuteDesc& desc) {
    if (!m_VulkanDevice || !m_VulkanDevice->SupportsDGC()) {
        HE_CORE_WARN("ExecuteGeneratedCommands: 设备不支持 DGC");
        return;
    }

    VkCommandBuffer cb = m_CmdBuffers[m_FrameIndex];
    const auto& dgcFuncs = m_VulkanDevice->GetDGCFuncs();

    VkIndirectCommandsLayoutEXT layout =
        reinterpret_cast<VkIndirectCommandsLayoutEXT>(desc.indirectCommandsLayout);
    VkIndirectExecutionSetEXT executionSet =
        reinterpret_cast<VkIndirectExecutionSetEXT>(desc.indirectExecutionSet);

    VkGeneratedCommandsInfoEXT genInfo{};
    genInfo.sType              = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT;
    genInfo.shaderStages       = VK_SHADER_STAGE_VERTEX_BIT
                                 | VK_SHADER_STAGE_FRAGMENT_BIT;
    genInfo.indirectExecutionSet   = executionSet;
    genInfo.indirectCommandsLayout = layout;
    genInfo.indirectAddress        = desc.sequencesBufferAddr;
    genInfo.indirectAddressSize    = desc.maxSequenceCount * kDGCDrawIndexedIndirectStride;
    genInfo.preprocessAddress      = desc.preprocessBufferAddr;
    genInfo.preprocessSize         = desc.preprocessBufferSize;
    genInfo.maxSequenceCount       = desc.maxSequenceCount;
    genInfo.sequenceCountAddress   = desc.sequenceCountAddr;
    genInfo.maxDrawCount           = desc.maxDrawCount;

    dgcFuncs.vkCmdExecuteGeneratedCommandsEXT(cb, VK_FALSE, &genInfo);
}

void VulkanCommandList::SetPushConstants(u32 offset, u32 size, const void* data) {
    VkShaderStageFlags stage;
    if (m_CurrentBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
        stage = VK_SHADER_STAGE_COMPUTE_BIT;
    } else if (m_CurrentBindPoint == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR) {
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

// ── GPU 通用查询（BeginQuery / EndQuery）──
void VulkanCommandList::BeginQuery(IRHIQueryPool* pool, u32 queryIndex) {
    auto* vkPool = static_cast<VulkanQueryPool*>(pool);
    vkCmdBeginQuery(m_CmdBuffers[m_FrameIndex], vkPool->GetHandle(), queryIndex, 0);
}

void VulkanCommandList::EndQuery(IRHIQueryPool* pool, u32 queryIndex) {
    auto* vkPool = static_cast<VulkanQueryPool*>(pool);
    vkCmdEndQuery(m_CmdBuffers[m_FrameIndex], vkPool->GetHandle(), queryIndex);
}

// ── Debug Label（VK_EXT_debug_utils 调试标签）──
void VulkanCommandList::BeginDebugLabel(const char* name, const float color[4]) {
    if (!m_VulkanDevice) return;
    auto fn = m_VulkanDevice->GetCmdBeginDebugLabelFn();
    if (!fn) return;

    VkDebugUtilsLabelEXT label{};
    label.sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = name;
    if (color) {
        label.color[0] = color[0]; label.color[1] = color[1];
        label.color[2] = color[2]; label.color[3] = color[3];
    } else {
        // 默认蓝色：便于在 RenderDoc 中区分不同 Pass
        label.color[0] = 0.3f; label.color[1] = 0.5f;
        label.color[2] = 0.9f; label.color[3] = 1.0f;
    }
    fn(m_CmdBuffers[m_FrameIndex], &label);
}

void VulkanCommandList::EndDebugLabel() {
    if (!m_VulkanDevice) return;
    auto fn = m_VulkanDevice->GetCmdEndDebugLabelFn();
    if (fn) fn(m_CmdBuffers[m_FrameIndex]);
}

void VulkanCommandList::InsertDebugLabel(const char* name, const float color[4]) {
    if (!m_VulkanDevice) return;
    auto fn = m_VulkanDevice->GetCmdInsertDebugLabelFn();
    if (!fn) return;

    VkDebugUtilsLabelEXT label{};
    label.sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = name;
    if (color) {
        label.color[0] = color[0]; label.color[1] = color[1];
        label.color[2] = color[2]; label.color[3] = color[3];
    }
    fn(m_CmdBuffers[m_FrameIndex], &label);
}

// ============================================================
// Copy
// ============================================================

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

    // 根据纹理格式确定正确的深度/颜色 layout 和 aspect
    Format fmt = vkTex->GetFormat();
    bool isDepth = (fmt == Format::D32_FLOAT || fmt == Format::D24_UNORM_S8_UINT ||
                    fmt == Format::D16_UNORM || fmt == Format::D32_FLOAT_S8_UINT);
    VkImageAspectFlags aspect = isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

    VkImageMemoryBarrier imageBarrier{};
    imageBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imageBarrier.srcAccessMask       = ToVkAccessFlags(srcState);
    imageBarrier.dstAccessMask       = ToVkAccessFlags(dstState);
    imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.image = vkTex->GetImage();

    // 深度纹理不能使用 COLOR_ATTACHMENT_OPTIMAL layout，需替换为深度对应 layout
    auto fixLayout = [&](ResourceState state) -> VkImageLayout {
        VkImageLayout layout = ToVkImageLayout(state);
        if (isDepth && layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        return layout;
    };
    imageBarrier.oldLayout = fixLayout(srcState);
    imageBarrier.newLayout = fixLayout(dstState);
    imageBarrier.subresourceRange = {
        aspect,
        0, vkTex->GetMipLevels(),
        0, vkTex->GetArrayLayers()
    };

    vkCmdPipelineBarrier(m_CmdBuffers[m_FrameIndex],
        ToVkPipelineStageFlags(srcStage), ToVkPipelineStageFlags(dstStage),
        0, 0, nullptr, 0, nullptr, 1, &imageBarrier);
}

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
// Ray Tracing 命令
// ============================================================

void VulkanCommandList::BuildBLAS(IRHIAccelerationStructure* blas, IRHIBuffer* scratchBuffer,
                                   const BLASBuildDesc& desc, bool update) {
    auto* vkBLAS  = static_cast<VulkanAccelerationStructure*>(blas);
    auto* vkScratch = static_cast<VulkanBuffer*>(scratchBuffer);
    auto& rt = m_VulkanDevice->GetRTDispatch();
    VkCommandBuffer cb = m_CmdBuffers[m_FrameIndex];

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
        vkGeo.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
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

    VkAccelerationStructureGeometryKHR geoInfo{};
    geoInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geoInfo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geoInfo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geoInfo.geometry.instances.arrayOfPointers = VK_FALSE;
    geoInfo.geometry.instances.data.deviceAddress = vkInstBuf->GetDeviceAddress();

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

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount  = instanceCount;
    range.primitiveOffset = 0;
    range.firstVertex     = 0;
    range.transformOffset = 0;

    VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
    rt.cmdBuildAS(cb, 1, &buildInfo, &pRange);

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

    auto* vkBuf = static_cast<VulkanBuffer*>(sbt.buffer);
    u64 baseAddr = vkBuf->GetDeviceAddress();
    u64 bufSize  = vkBuf->GetSize();

    auto makeRegion = [&](const SBTSlot& slot, u64 nextOffset) -> VkStridedDeviceAddressRegionKHR {
        VkStridedDeviceAddressRegionKHR region{};
        if (slot.stride == 0) return region;
        region.deviceAddress = baseAddr + slot.handleOffset;
        region.stride        = slot.stride;
        u64 endOffset = (nextOffset > slot.handleOffset) ? nextOffset : bufSize;
        region.size = (endOffset > slot.handleOffset) ? (endOffset - slot.handleOffset) : slot.stride;
        return region;
    };

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

} // namespace he::rhi
