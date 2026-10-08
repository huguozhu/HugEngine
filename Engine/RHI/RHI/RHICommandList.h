#pragma once

#include "Core/Assert.h"
#include "Core/Types.h"

#include <cstring>
#include <new>
#include <vector>

// ============================================================
// RHICommandList —— RHI 命令流的**记录端契约**（方案 B §12.7 / 阶段 0 任务 T0.6）
//
// 【为什么阶段 0 就要落它】方案 §12.1 的分水岭在 T2.6：如果 `RenderThreadContext` 将来要"吐命令流"，
// 那么**记录端的形状必须现在就定**——命令载荷要么类型擦除 + 内联存储，要么事后改造整个渲染层
// （200+ 处资源持有者）。本文件只定义契约与载荷形态，**不接入任何生产路径**：
// 记录下来的命令由谁执行、在哪根线程翻译，都是阶段 2（T2.6）与 §12 的 A-1/A-2 的事。
//
// 【为什么不用 std::function】每帧可能有数千条命令，逐条 `std::function` 会带来堆分配 + 间接调用；
// UE 的 `FRHICommand` 用"类型擦除 + 内联参数"正是这个原因（方案 §12.10 明确列为"不建议的做法"）。
// 所以：
//   · 小参数（≤ `kInlinePayloadBytes`）直接内联进命令记录，零分配；
//   · 大块数据（常量缓冲、上传源、SBT 记录）写本列表私有的 **arena**，执行期按偏移读取。
//
// 【交接纪律（铁律 2）】记录完成后，本列表被视为只读：渲染线程（将来是 RHI 线程）读取时，
// 记录方不得再改任何载荷或 arena 内容；需要复用就按帧轮换列表实例。
// ============================================================

namespace he::rhi {

/// 单条命令可内联的参数字节数（与 UE `FRHICommand` 同量级；超出部分走 arena）
inline constexpr u32 kInlinePayloadBytes = 64;

/// arena 默认容量（1 MiB）与块基址对齐。
/// 【为什么 arena 要有自己的对齐块】执行端会按 16B（`float4`）甚至更大对齐读取 arena 里的内联数据，
/// 所以对齐必须建立在**绝对地址**上。`std::vector<u8>` 只保证元素对齐 1B，基址在堆上何时 64B 对齐
/// 全看分配器 ⇒ 只对齐"偏移量"是不够的（第一版就是这么写的，被单测当场抓出：p2 % 64 == 48）。
/// 这里改成自己持有一块**固定容量的 64B 对齐**内存：永不重分配 ⇒ 已发出的指针始终有效；
/// 容量耗尽即断言（真正的可增长块分配器随 §12 的 A-1 落地，本阶段只需要契约正确）。
inline constexpr u64 kDefaultArenaCapacityBytes = 1u << 20;
inline constexpr u64 kArenaBlockAlignment       = 64;

/// 命令类型。**这里只登记骨架用到的代表值**：真正的完整清单随 §12 的 A-1（命令流落地）一次定稿，
/// 现在多写类型只会变成"没人用的枚举"。
enum class RHICommandType : u32 {
    Invalid           = 0,
    SetPipeline       = 1,
    SetDescriptorSet  = 2,
    DrawIndexed       = 3,
    Dispatch          = 4,
    PipelineBarrier   = 5,
    CopyBuffer        = 6,
    MarkSubmitBoundary = 7,
};

/// 一条命令的载荷：类型擦除 + **内联存储**。
/// 约定：`payloadSize <= kInlinePayloadBytes` 时参数在 `inlinePayload` 内；超过时 `payloadSize`
/// 记 0 并把实际数据放在 arena，偏移写在 `arenaOffset`（由 `EnqueueArena` 生成）。
struct RHICommand {
    RHICommandType type         = RHICommandType::Invalid;
    u32            payloadSize  = 0;    // 内联参数的实际字节数（0 表示走 arena）
    u64            arenaOffset  = 0;    // 走 arena 时的偏移（相对 arena 起点）
    u64            arenaSize    = 0;    // 走 arena 时的字节数
    alignas(16) u8 inlinePayload[kInlinePayloadBytes] = {};
};

/// 命令列表：命令数组 + 参数 arena。记录端写，执行端读；交接后只读（铁律 2）。
class RHICommandList {
public:
    explicit RHICommandList(u64 arenaCapacity = kDefaultArenaCapacityBytes)
        : m_ArenaCapacity(arenaCapacity < kArenaBlockAlignment ? kArenaBlockAlignment : arenaCapacity) {
        // 64B 对齐分配：保证 base 本身满足最大对齐需求，之后任何对齐偏移都能得到对齐地址
        m_Arena = static_cast<u8*>(::operator new(static_cast<usize>(m_ArenaCapacity),
                                                  std::align_val_t{kArenaBlockAlignment}));
    }

    ~RHICommandList() {
        if (m_Arena) ::operator delete(m_Arena, std::align_val_t{kArenaBlockAlignment});
    }

    RHICommandList(const RHICommandList&)            = delete;
    RHICommandList& operator=(const RHICommandList&) = delete;

    /// 记录一条**内联**命令：参数按值拷进命令记录（禁止传会被继续修改的指针指向的数据，
    /// 因为执行时那些数据可能已经变了 —— 铁律 2）。
    template <typename TPayload>
    void Enqueue(RHICommandType type, const TPayload& payload) {
        static_assert(sizeof(TPayload) <= kInlinePayloadBytes,
                      "参数超过 64B：请改用 EnqueueArena（大块数据走本列表私有 arena）");
        static_assert(alignof(TPayload) <= kInlinePayloadBytes, "参数对齐超过内联上限");

        RHICommand cmd;
        cmd.type        = type;
        cmd.payloadSize = static_cast<u32>(sizeof(TPayload));
        std::memcpy(cmd.inlinePayload, &payload, sizeof(TPayload));
        m_Commands.push_back(cmd);
    }

    /// 记录一条**大块**命令：先把数据写进 arena（按 `alignof(TPayload)` 对齐），再记下偏移与长度。
    template <typename TPayload>
    void EnqueueArena(RHICommandType type, const TPayload& payload) {
        u8* dst = static_cast<u8*>(AllocArena(sizeof(TPayload), alignof(TPayload)));
        std::memcpy(dst, &payload, sizeof(TPayload));

        RHICommand cmd;
        cmd.type        = type;
        cmd.payloadSize = 0u;
        cmd.arenaOffset = static_cast<u64>(dst - m_Arena);
        cmd.arenaSize   = sizeof(TPayload);
        m_Commands.push_back(cmd);
    }

    /// 在 arena 里分配 `size` 字节（按 `alignment` 对齐）并返回起始地址。
    /// 只增不减；`Reset()` 把已用长度归零但**保留这一块内存**（块内数据仍可被覆写，但不再有效）。
    [[nodiscard]] void* AllocArena(u64 size, u64 alignment = 16) {
        if (alignment == 0u) alignment = 1u;
        if (alignment > kArenaBlockAlignment) alignment = kArenaBlockAlignment;   // 块基址只保证 64B
        const u64 aligned = (m_ArenaUsed + alignment - 1u) / alignment * alignment;
        HE_ASSERT_MSG(aligned + size <= m_ArenaCapacity,
                      "RHICommandList：arena 容量耗尽（本阶段容量固定，扩容随 §12 的 A-1 落地）");
        m_ArenaUsed = aligned + size;
        return m_Arena + aligned;
    }

    /// 标记本列表是"本帧的提交边界"（将来由 RHI 线程在翻译完成后执行一次 submit）
    void MarkSubmitBoundary() {
        Enqueue(RHICommandType::MarkSubmitBoundary, kNoPayload);
        m_HasSubmit = true;
    }

    // --- 执行端（阶段 2 / §12 的 A-1）读取用 ---

    [[nodiscard]] usize         CommandCount() const { return m_Commands.size(); }
    [[nodiscard]] const RHICommand& CommandAt(usize index) const { return m_Commands[index]; }
    [[nodiscard]] bool          HasSubmitBoundary() const { return m_HasSubmit; }
    [[nodiscard]] u64           ArenaBytes() const { return m_ArenaUsed; }
    [[nodiscard]] u64           ArenaCapacity() const { return m_ArenaCapacity; }
    [[nodiscard]] u8*           ArenaData() { return m_Arena; }

    /// 按类型读回内联载荷（执行端用）。长度不匹配时返回 nullptr —— 用返回值而不是断言，
    /// 是因为执行端将来要能对**越界/损坏的命令流**容错（那一层不允许直接 abort）。
    template <typename TPayload>
    [[nodiscard]] const TPayload* PayloadAt(usize index) const {
        if (index >= m_Commands.size()) return nullptr;
        const RHICommand& cmd = m_Commands[index];
        if (cmd.payloadSize != sizeof(TPayload)) return nullptr;
        return reinterpret_cast<const TPayload*>(cmd.inlinePayload);
    }

    /// 按类型读回 arena 载荷（执行端用）；同样以 nullptr 表示不匹配
    template <typename TPayload>
    [[nodiscard]] const TPayload* ArenaPayloadAt(usize index) const {
        if (index >= m_Commands.size()) return nullptr;
        const RHICommand& cmd = m_Commands[index];
        if (cmd.payloadSize != 0u || cmd.arenaSize != sizeof(TPayload)) return nullptr;
        if (cmd.arenaOffset + cmd.arenaSize > m_ArenaUsed) return nullptr;
        return reinterpret_cast<const TPayload*>(m_Arena + cmd.arenaOffset);
    }

    /// 复用本列表（下一帧重新记录）：清空命令、把 arena 已用长度归零，**保留容量与块地址**
    /// （因此已发出的指针依旧有效，只是内容不再保证）。
    void Reset() {
        m_Commands.clear();
        m_ArenaUsed = 0u;
        m_HasSubmit = false;
    }

private:
    /// `MarkSubmitBoundary` 用的空载荷（避免为它单独开一个分支）
    struct NoPayload {};
    static constexpr NoPayload kNoPayload{};

    std::vector<RHICommand> m_Commands;
    u8*                     m_Arena         = nullptr;   // 固定容量、64B 对齐
    u64                     m_ArenaCapacity = 0;
    u64                     m_ArenaUsed     = 0;
    bool                    m_HasSubmit     = false;     // 是否已标记提交边界
};

} // namespace he::rhi
