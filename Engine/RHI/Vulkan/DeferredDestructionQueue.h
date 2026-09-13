// ============================================================
// DeferredDestructionQueue.h — GPU 资源延迟销毁队列
//
// 核心问题：Vulkan 资源（VkPipeline、VkFramebuffer、VkRenderPass、
// VkDescriptorSet、VkBuffer、VkImage 等）不能在 GPU 仍在使用时销毁。
// 本类提供统一的 N 帧延迟销毁机制，替代各处散布的 ad-hoc 管理。
//
// 使用方式：
//   1. 在 VulkanDevice 中创建 DeferredDestructionQueue 实例
//   2. 每帧开始时调用 Advance()（在 fence 等待后）
//   3. 任何 GPU 资源需要销毁时调用 Enqueue(deleter)
//   4. Shutdown 时调用 FlushAll() 立即销毁所有待处理资源
//
// 生命周期保证：
//   - Enqueue 时资源进入"当前帧槽位"
//   - 经过 kMaxFramesInFlight 次 Advance() 后，槽位中的资源被销毁
//   - 此时 GPU 已确认完成该帧的所有命令（fence 已 signal）
//
// 参考修复记录：
//   - 26a051c: PSO 延迟销毁（GPU 仍在使用 VkRenderPass）
//   - 8d6dfa9: PSORecord 指针悬空（vector push_back 重分配）
//   - 8bc2766: VMA 崩溃（纹理缓存在 Device 前释放）
//   - b1e243e: 离屏 Framebuffer 提前销毁（GPU 未完成渲染）
//   - VulkanCommandList::m_PendingFBs: Framebuffer ad-hoc 延迟销毁
// ============================================================
#pragma once

#include <functional>
#include <vector>

#include "RHI/Types.h"   // kMaxFramesInFlight

namespace he::rhi {

class DeferredDestructionQueue {
public:
    /// 槽位数：必须 **大于** kMaxFramesInFlight。
    ///
    /// 原因（一次实测事故）：每帧只调用一次 Advance()，资源在"入队后经过
    /// kMaxFramesInFlight 次 Advance"才被销毁——若槽位数正好等于飞行帧数，
    /// 等 fence 的时机与销毁时机之间只剩 1 帧余量，一旦出现"一帧内多次推进"
    /// （例如多个命令列表各自 Begin）就会退化为**同帧销毁**：命令缓冲仍引用
    /// 刚被销毁的 VkFramebuffer → 校验层报
    /// "vkCmdBeginRenderPass(): ... command buffer ... was in an invalid state ...
    ///  VkFramebuffer ... was destroyed"（06.GILab 实测 110 次/8 秒）。
    /// 因此取 2 × kMaxFramesInFlight，留出一倍余量。
    static constexpr u32 kSlots = kMaxFramesInFlight * 2;

    DeferredDestructionQueue() = default;

    /// 入队一个延迟销毁操作。
    /// @param deleter  销毁函数，在 N 帧后执行（N = kMaxFramesInFlight）
    /// @note 内部捕获所有需要的资源句柄，确保生命周期独立于调用方
    void Enqueue(std::function<void()> deleter) {
        m_Queue[m_WriteIndex].push_back(std::move(deleter));
    }

    /// 每帧开始时调用：将当前槽位轮转到下一个，
    /// 并执行最老槽位的所有销毁操作（此时 GPU 已确认完成）。
    void Advance() {
        // 轮转到下一个槽位
        m_WriteIndex = (m_WriteIndex + 1) % kSlots;
        // 销毁最老槽位中的所有资源（已安全完成 kSlots-1 帧）
        ExecuteSlot(m_WriteIndex);
    }

    /// 立即执行所有槽位的销毁操作（用于 Shutdown）。
    /// 调用前应确保 GPU 已完成所有工作（已调用 vkDeviceWaitIdle）。
    void FlushAll() {
        for (u32 i = 0; i < kSlots; ++i) {
            ExecuteSlot(i);
            m_Queue[i].clear();
        }
    }

    /// 获取当前写入槽位索引
    u32 GetWriteIndex() const { return m_WriteIndex; }

private:
    void ExecuteSlot(u32 index) {
        for (auto& op : m_Queue[index]) {
            if (op) op();
        }
        m_Queue[index].clear();
    }

    // 多槽位环形缓冲：每个槽位存储本帧入队的销毁操作
    // 入队进当前槽位；经过 kSlots-1 次 Advance() 后该槽位被执行
    // （kSlots = 2 × kMaxFramesInFlight，保证销毁时 GPU 早已完成引用它的帧）
    std::vector<std::function<void()>> m_Queue[kSlots];
    u32 m_WriteIndex = 0;
};

} // namespace he::rhi
