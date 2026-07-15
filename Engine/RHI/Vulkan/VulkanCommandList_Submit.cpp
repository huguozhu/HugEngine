// VulkanCommandList_Submit.cpp — Submit 与跨队列同步
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

    // ResourceState → VkImageLayout / VkAccessFlags 辅助映射
    auto toLayout = [](ResourceState s) {
        u32 v = u32(s);
        if (v & u32(ResourceState::DepthStencilWrite)) return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        if (v & u32(ResourceState::DepthStencilRead))  return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        if (v & u32(ResourceState::RenderTarget))       return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (v & u32(ResourceState::ShaderResource))     return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (v & u32(ResourceState::UnorderedAccess))    return VK_IMAGE_LAYOUT_GENERAL;
        return VK_IMAGE_LAYOUT_UNDEFINED;
    };
    auto toAccess = [](ResourceState s) {
        VkAccessFlags f = 0;
        u32 v = u32(s);
        if (v & u32(ResourceState::DepthStencilWrite)) f |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        if (v & u32(ResourceState::DepthStencilRead))  f |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        if (v & u32(ResourceState::ShaderResource))     f |= VK_ACCESS_SHADER_READ_BIT;
        if (v & u32(ResourceState::RenderTarget))       f |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        if (v & u32(ResourceState::UnorderedAccess))    f |= VK_ACCESS_SHADER_WRITE_BIT;
        return f ? f : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    };

    VkImageMemoryBarrier imageBarrier{};
    imageBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imageBarrier.srcAccessMask       = toAccess(currentState);
    imageBarrier.dstAccessMask       = toAccess(newState);
    imageBarrier.oldLayout           = toLayout(currentState);
    imageBarrier.newLayout           = toLayout(newState);
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

    auto toAccess = [](ResourceState s) {
        VkAccessFlags f = 0;
        u32 v = u32(s);
        if (v & u32(ResourceState::DepthStencilWrite)) f |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        if (v & u32(ResourceState::DepthStencilRead))  f |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        if (v & u32(ResourceState::ShaderResource))     f |= VK_ACCESS_SHADER_READ_BIT;
        if (v & u32(ResourceState::RenderTarget))       f |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        if (v & u32(ResourceState::UnorderedAccess))    f |= VK_ACCESS_SHADER_WRITE_BIT;
        return f ? f : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    };

    VkBufferMemoryBarrier bufferBarrier{};
    bufferBarrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bufferBarrier.srcAccessMask       = toAccess(currentState);
    bufferBarrier.dstAccessMask       = toAccess(newState);
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
    QueueOwnershipTransfer(texture, m_QueueType, dstQueue,
                           ResourceState::ShaderResource, ResourceState::ShaderResource);
}

void VulkanCommandList::AcquireFromQueue(IRHITexture* texture, QueueType srcQueue) {
    QueueOwnershipTransfer(texture, srcQueue, m_QueueType,
                           ResourceState::ShaderResource, ResourceState::ShaderResource);
}

// ============================================================
// Timeline Semaphore 集成
// ============================================================

void VulkanCommandList::SetTimelineSignal(RHIFenceHandle fence, u64 value) {
    if (!m_VulkanDevice || fence == kInvalidFence) return;
    m_TimelineSignalSem = m_VulkanDevice->ResolveFenceSemaphore(fence);
    m_TimelineSignalVal = value;
}

void VulkanCommandList::SetTimelineWait(RHIFenceHandle fence, u64 value) {
    if (!m_VulkanDevice || fence == kInvalidFence) return;
    m_TimelineWaitSem = m_VulkanDevice->ResolveFenceSemaphore(fence);
    m_TimelineWaitVal = value;
}

// ============================================================
// Submit — 提交命令缓冲到 GPU 队列
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

    // 构建二进制 + Timeline 信号量列表
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

    VkTimelineSemaphoreSubmitInfo timelineInfo{};
    timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timelineInfo.waitSemaphoreValueCount   = waitCount;
    timelineInfo.pWaitSemaphoreValues      = timelineWaitVals;
    timelineInfo.signalSemaphoreValueCount = signalCount;
    timelineInfo.pSignalSemaphoreValues    = timelineSignalVals;

    VkSubmitInfo submitInfo{};
    submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pNext                = &timelineInfo;
    submitInfo.waitSemaphoreCount   = waitCount;
    submitInfo.pWaitSemaphores      = allWaitSems;
    submitInfo.pWaitDstStageMask    = waitStages;
    submitInfo.commandBufferCount   = 1;
    submitInfo.pCommandBuffers      = &cb;
    submitInfo.signalSemaphoreCount = signalCount;
    submitInfo.pSignalSemaphores    = allSignalSems;

    // 重置当前帧栅栏并提交
    vkResetFences(m_Device, 1, &m_Fences[m_FrameIndex]);
    vkQueueSubmit(m_Queue, 1, &submitInfo, m_Fences[m_FrameIndex]);

    // 清除 Timeline 信号量状态
    m_TimelineSignalSem = VK_NULL_HANDLE;
    m_TimelineSignalVal = 0;
    m_TimelineWaitSem   = VK_NULL_HANDLE;
    m_TimelineWaitVal   = 0;

    m_FrameIndex = (m_FrameIndex + 1) % kMaxFramesInFlight;
}

} // namespace he::rhi
