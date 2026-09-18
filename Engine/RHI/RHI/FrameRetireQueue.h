#pragma once

#include "RHI/Types.h"   // kMaxFramesInFlight

#include <utility>
#include <vector>

// ============================================================
// FrameRetireQueue.h — 有界"N 帧延迟释放"队列（任务 23）
//
// 【要解决的问题】GPU 资源（纹理/SSBO）不能在"引用它的帧"还没结束时销毁，于此前各处用
//   "无界 vector 保活"顶着：旧资源永不释放，高频更新（文字每秒重栅格化、实例变换每帧变）
//   会让显存与 bindless 槽位持续增长。
//
// 【本类做的事】把"保活"换成"Ring + 有界延迟释放"：
//   Retire(资源) 进入当前槽位 → 每帧 Advance() 一次轮转并释放最老槽位。
//   槽位数固定 ⇒ 待释放数量有界；延迟 kSlots-1 帧 ⇒ 引用它的帧早已完成（fence 已 signal）。
//
// 槽位数取 2 × 飞行帧数：与 DeferredDestructionQueue 同源教训 —— 若槽位数正好等于飞行
// 帧数，"一帧内推进多次"会退化成同帧释放（见 DeferredDestructionQueue.h 的事故记录）。
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
        m_Slots[m_WriteIndex].clear();
    }

    /// 立即释放全部（Shutdown / 确保 GPU 已 idle 后使用）
    void FlushAll() {
        for (auto& slot : m_Slots) slot.clear();
        m_PendingCount = 0;
    }

    u32 GetPendingCount() const { return m_PendingCount; }
    static constexpr u32 GetSlotCount() { return kSlots; }

private:
    std::vector<T> m_Slots[kSlots];
    u32 m_WriteIndex   = 0;
    u32 m_PendingCount = 0;
};

} // namespace he::rhi
