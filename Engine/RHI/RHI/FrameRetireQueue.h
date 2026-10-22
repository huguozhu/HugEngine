#pragma once

#include "RHI/Types.h"   // kMaxFramesInFlight

#include <functional>
#include <utility>
#include <vector>

// ============================================================
// FrameRetireQueue.h — 有界"N 帧延迟释放"队列（任务 23）
//
// 【要解决的问题】GPU 资源（纹理/SSBO）不能在"引用它的帧还没结束"时销毁，此前各处用
//   "无界 vector 保活"顶着：旧资源永不释放，高频更新（文字每秒重栅格化、实例变换每帧变）
//   会让显存与 bindless 槽位持续增长。
//
// 【本类做的事】把"保活"换成"Ring + 有界延迟释放"：
//   Retire(资源) 进入当前槽位 → 每帧 Advance() 一次轮转并释放最老槽位。
//   槽位数固定 ⇒ 待释放数量有界；延迟 kSlots-1 帧 ⇒ 引用它的帧早已完成（fence 已 signal）。
//
// 槽位数取 2 × 飞行帧数：与 DeferredDestructionQueue 同源教训 —— 若槽位数正好等于飞行
//   帧数，"一帧内推进多次"会退化成同帧释放（见 DeferredDestructionQueue.h 的事故记录）。
//
// 【A-3 扩展】资源句柄（RHIBufferHandle / RHITextureHandle …）不随容器析构自动释放，
//   所以本类加了一个可选的"释放器"：设置后，槽位被清空之前会逐个回调它。
//   未设置时行为与原来完全一致（`unique_ptr` 等自管理类型不需要）。
// ============================================================

namespace he::rhi {

template <typename T>
class FrameRetireQueue {
public:
    static constexpr u32 kSlots = kMaxFramesInFlight * 2;

    /// 入队一个待释放资源（进入当前写入槽位）
    void Retire(T&& value) {
        m_Slots[m_WriteIndex].push_back(std::move(value));
        ++m_PendingCount;
    }

    /// 帧边界推进：轮转槽位并释放最老槽位中的资源
    /// 【调用约定】每帧调用一次；应在"本帧入队新资源"**之前**调用
    void Advance() {
        m_WriteIndex = (m_WriteIndex + 1) % kSlots;
        m_PendingCount -= (u32)m_Slots[m_WriteIndex].size();
        ReleaseSlot(m_Slots[m_WriteIndex]);
    }

    /// 立即释放全部（Shutdown / 确保 GPU 已 idle 后使用）
    void FlushAll() {
        for (auto& slot : m_Slots) ReleaseSlot(slot);
        m_PendingCount = 0;
    }

    /// 【A-3】为句柄类型设置释放器（例如 `[dev](RHIBufferHandle& h){ dev->DestroyBufferHandle(h); }`）。
    /// 未设置时槽位清空不会动资源本身 —— 与迁移前的 `unique_ptr` 语义一致。
    void SetReleaser(std::function<void(T&)> releaser) { m_Releaser = std::move(releaser); }

    u32 GetPendingCount() const { return m_PendingCount; }
    static constexpr u32 GetSlotCount() { return kSlots; }

private:
    /// 清空一个槽位：先按需回调释放器，再清容器
    void ReleaseSlot(std::vector<T>& slot) {
        if (m_Releaser) {
            for (auto& value : slot) m_Releaser(value);
        }
        slot.clear();
    }

    std::vector<T> m_Slots[kSlots];
    std::function<void(T&)> m_Releaser;   // A-3：句柄释放器（可选，默认空 = 原行为）
    u32 m_WriteIndex   = 0;
    u32 m_PendingCount = 0;
};

} // namespace he::rhi
