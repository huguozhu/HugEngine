#pragma once

#include "RHI/Types.h"   // kMaxFramesInFlight

#include <vector>

// ============================================================
// BindlessSlotRing.h — bindless 槽位环形分配器（任务 23）
//
// 【要解决的问题】bindless 堆原来是 **append-only**：每次 Register* 都往后追加一个槽位。
// TextRender 每次文字变化、InstancedMesh 每次变换变化都会重新注册，于是
//   ① 描述符数组无限增长（纹理/SSBO 数组有容量上限）；
//   ② 调用方只能"把旧资源一直保活"（销毁就会留下悬垂描述符）。
//
// 【本类做的事】把"槽位"变成可回收的环形资源：
//   Acquire() 优先复用空闲槽；Release(slot) 只登记"待回收"，**要过 graceFrames 帧**
//   才回到空闲表 —— 因为可能有最多 kMaxFramesInFlight 个"在飞"的帧仍在使用该槽位里的
//   描述符（资源本身也由调用方的延迟释放队列保住 N 帧，见 FrameRetireQueue.h）。
//
// 【为什么不在 Release 时立即清空描述符】立即改写会让仍在执行的帧采样到错误的资源
//   （画面闪烁/数据错乱），所以保护期内槽位内容保持原样，只是不再分配给新资源。
//
// 与后端无关（不依赖 Vulkan），策略本身可被 doctest 直接覆盖。
// ============================================================

namespace he::rhi {

class BindlessSlotRing {
public:
    /// @param graceFrames 槽位释放后需要经过的帧数（取飞行帧数，保证引用它的帧已完成）
    explicit BindlessSlotRing(u32 graceFrames = kMaxFramesInFlight)
        : m_GraceFrames(graceFrames == 0 ? 1u : graceFrames) {}

    /// 申请槽位下标。
    /// · 空闲表非空 ⇒ **复用**（返回值 < 调用前的 GetSlotCount()）
    /// · 空闲表为空 ⇒ **新分配**（返回值 == 调用前的 GetSlotCount()，调用方需 push_back 扩容）
    u32 Acquire() {
        if (!m_Free.empty()) {
            const u32 slot = m_Free.back();   // 后进先出：先回收的槽先复用
            m_Free.pop_back();
            return slot;
        }
        return m_SlotCount++;
    }

    /// 登记释放（延迟 graceFrames 帧后才可复用）。越界 / 重复释放 / 已是空闲：安全忽略。
    void Release(u32 slot) {
        if (slot >= m_SlotCount) return;
        for (const auto& p : m_Pending)
            if (p.slot == slot) return;          // 已在待回收表：忽略
        for (u32 f : m_Free)
            if (f == slot) return;               // 已是空闲槽：忽略
        m_Pending.push_back({ slot, m_Frame });
    }

    /// 帧边界推进（每帧一次）：把保护期已过的槽位放回空闲表
    void BeginFrame() {
        ++m_Frame;
        for (usize i = 0; i < m_Pending.size();) {
            if (m_Frame >= m_Pending[i].frame + m_GraceFrames) {
                m_Free.push_back(m_Pending[i].slot);
                m_Pending[i] = m_Pending.back();   // 交换删除（无序回收，顺序无意义）
                m_Pending.pop_back();
            } else {
                ++i;
            }
        }
    }

    // --- 查询（调试 / 判据 / 统计）---
    u32 GetSlotCount()        const { return m_SlotCount; }                 // 已分配槽位总数（数组长度）
    u32 GetFreeSlotCount()    const { return (u32)m_Free.size(); }          // 可立即复用
    u32 GetPendingFreeCount() const { return (u32)m_Pending.size(); }       // 保护期未过
    u32 GetGraceFrames()      const { return m_GraceFrames; }

private:
    struct PendingFree {
        u32 slot  = 0;
        u64 frame = 0;   // 释放时所在的帧号
    };

    u32 m_GraceFrames;
    u32 m_SlotCount = 0;     // 从未复用过的"新高水位"（数组长度）
    u64 m_Frame     = 0;     // 帧计数器（BeginFrame 自增，与设备帧号解耦）
    std::vector<u32> m_Free;
    std::vector<PendingFree> m_Pending;
};

} // namespace he::rhi
