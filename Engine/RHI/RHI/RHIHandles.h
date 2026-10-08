#pragma once

#include "Core/Types.h"

#include <vector>

// ============================================================
// RHIHandles —— 资源句柄化（方案 B §12.5 / 阶段 0 任务 T0.7）
//
// 【为什么阶段 0 就要做】方案 §12.5 把它列为"必须提前定的 5 个决定"之首：现在 `IRHIDevice::Create*`
// 返回 `std::unique_ptr<T>`，各 Pass/管线用 `unique_ptr` 成员持有 —— 这是"同步立即给所有权"的语义。
// §12 的 A-3（RHI 线程落地）要求创建发生在 RHI 线程、渲染线程只拿句柄；若等到那时再改，就要动全工程
// 200+ 处持有者。因此本阶段只做两件事：
//   ① 定义 `RHIBufferHandle` / `RHITextureHandle`（含 **generation**，用于检测悬挂引用）；
//   ② 提供一块**句柄表**（`RHIResourceTable`），把"句柄 → 资源"的解析与代次校验收在一处。
// `unique_ptr` 成员作为兼容层保留，旧代码渐进迁移（本阶段不强制改造任何现有持有者）。
//
// 【generation 是干什么的】槽位会被回收复用。若句柄只有下标，那么"旧句柄"在槽位被新资源占用后会
// 静默指向**另一个资源**（最危险的悬挂形态：不崩、画面莫名错乱）。带代次后，`Remove` 会提升槽位代次，
// 旧句柄一律解析为 nullptr ⇒ 错误立刻暴露成"资源为空"而不是"用错资源"。
// ============================================================

namespace he::rhi {

/// 缓冲区句柄。`index == 0` 表示无效（槽位 0 永不分配），`generation` 用于代次校验。
struct RHIBufferHandle {
    u32 index      = 0;
    u32 generation = 0;

    [[nodiscard]] bool IsValid() const { return index != 0u; }
    friend bool operator==(RHIBufferHandle a, RHIBufferHandle b) {
        return a.index == b.index && a.generation == b.generation;
    }
    friend bool operator!=(RHIBufferHandle a, RHIBufferHandle b) { return !(a == b); }
};

/// 纹理句柄。语义与缓冲区句柄一致（分开两个类型是为了让"传错资源种类"变成编译错误）。
struct RHITextureHandle {
    u32 index      = 0;
    u32 generation = 0;

    [[nodiscard]] bool IsValid() const { return index != 0u; }
    friend bool operator==(RHITextureHandle a, RHITextureHandle b) {
        return a.index == b.index && a.generation == b.generation;
    }
    friend bool operator!=(RHITextureHandle a, RHITextureHandle b) { return !(a == b); }
};

/// 句柄表：句柄 ⇄ 资源指针的唯一解析入口。
/// 【线程约束】它本身不做加锁：按方案 §3 铁律 1，RHI 只属于渲染线程（§12 之后是 RHI 线程），
/// 因此解析与增删都发生在同一根线程上；将来若要跨线程读，加锁应加在这一层而不是散在各 Pass。
template <typename THandle, typename TResource>
class RHIResourceTable {
public:
    /// 登记一个资源并返回句柄（复用空闲槽位；该槽位的代次保持不变，因此更早的旧句柄依然失效）
    [[nodiscard]] THandle Add(TResource* resource) {
        u32 index = 0;
        if (!m_FreeSlots.empty()) {
            index = m_FreeSlots.back();
            m_FreeSlots.pop_back();
        } else {
            m_Slots.push_back(Slot{});
            index = static_cast<u32>(m_Slots.size());   // 1 基下标：0 留给"无效"
        }
        Slot& slot = m_Slots[index - 1u];
        slot.resource = resource;
        if (slot.generation == 0u) slot.generation = 1u;   // 首次使用从 1 开始
        ++m_Live;
        return THandle{index, slot.generation};
    }

    /// 解析句柄：越界、代次过期（槽位已被回收复用）或槽位为空 ⇒ nullptr
    [[nodiscard]] TResource* Get(THandle handle) const {
        if (handle.index == 0u || handle.index > m_Slots.size()) return nullptr;
        const Slot& slot = m_Slots[handle.index - 1u];
        if (slot.generation != handle.generation) return nullptr;   // 代次过期：旧句柄一律失效
        return slot.resource;
    }

    /// 注销句柄指向的资源：提升槽位代次（使该句柄与所有更早的句柄失效）并回收槽位
    void Remove(THandle handle) {
        if (handle.index == 0u || handle.index > m_Slots.size()) return;
        Slot& slot = m_Slots[handle.index - 1u];
        if (slot.generation != handle.generation || slot.resource == nullptr) return;   // 已失效：幂等
        slot.resource = nullptr;
        ++slot.generation;                    // 关键：代次前进 ⇒ 旧句柄解析为 nullptr
        if (slot.generation == 0u) slot.generation = 1u;   // 回绕保护（不做 2^32 次回收就不会碰到）
        m_FreeSlots.push_back(handle.index);
        --m_Live;
    }

    [[nodiscard]] u32 LiveCount()    const { return m_Live; }
    [[nodiscard]] u32 SlotCount()    const { return static_cast<u32>(m_Slots.size()); }
    [[nodiscard]] u32 FreeSlotCount() const { return static_cast<u32>(m_FreeSlots.size()); }

private:
    struct Slot {
        TResource* resource   = nullptr;
        u32        generation = 1u;
    };

    std::vector<Slot> m_Slots;      // 1 基下标（m_Slots[i-1] 对应句柄下标 i）
    std::vector<u32>  m_FreeSlots;
    u32               m_Live = 0;
};

class IRHIBuffer;    // 前向声明：本头文件不依赖 Buffer.h / Texture.h 的实现细节
class IRHITexture;

using RHIBufferTable  = RHIResourceTable<RHIBufferHandle, IRHIBuffer>;
using RHITextureTable = RHIResourceTable<RHITextureHandle, IRHITexture>;

} // namespace he::rhi
