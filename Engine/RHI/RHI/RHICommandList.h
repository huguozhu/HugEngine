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

/// 命令类型（§12 的 A-1 落地时定稿的完整清单；只登记**有真实调用者**的类型）
enum class RHICommandType : u32 {
    Invalid           = 0,
    SetPipeline       = 1,
    SetDescriptorSet  = 2,
    DrawIndexed       = 3,
    Dispatch          = 4,
    PipelineBarrier   = 5,
    CopyBuffer        = 6,
    MarkSubmitBoundary = 7,
    // A-1 试点（Forward 的 draw 录制）需要的四类
    SetPushConstants   = 8,
    SetVertexBuffer    = 9,
    SetIndexBuffer     = 10,
    SetDrawDebugLabel  = 11,
    ExecuteSecondary   = 12,   // 主列表执行一条已录好的次级列表（MTCR 合并点）
    ExecuteIndirect    = 13,
    SetViewport        = 14,   // L4-3 batch2
    SetScissor         = 15,   // L4-3 batch2
    // L4-3 batch3/4：名字加 Cmd 前缀，避免与同名成员函数（BeginRenderPass/EndRenderPass/Draw/PipelineBarrier）冲突
    CmdBeginRenderPass    = 16,
    CmdEndRenderPass      = 17,
    CmdBeginOffscreenPass = 18,
    CmdEndOffscreenPass   = 19,
    CmdDraw               = 20,
    CmdPipelineBarrier    = 21,
    CmdDispatch            = 22,
    CmdCopyBuffer          = 23,
    CmdBeginOffscreenPassMRT = 24,
    CmdCopyTextureToBuffer = 28,
    CmdDispatchIndirect = 30,
    CmdCopyTextureToTexture = 32,
    CmdClearDepthStencil = 33,
    CmdBuildTLAS = 34,
    CmdBindRTPipeline = 35,
    CmdDrawIndexedIndirect = 25,
    CmdDrawIndexedIndirectCount = 26,
    // D3D12 命令流模式补齐的类型：帧主列表的全部录制调用都进流（回放侧逐一还原）
    CmdPipelineBarrierTexture   = 36,   // 带纹理的屏障（上面的 CmdPipelineBarrier 是全局屏障）
    CmdBufferBarrier            = 37,
    CmdBeginDebugLabel          = 38,   // 载荷同 SetDrawDebugLabel（内联短文本）
    CmdEndDebugLabel            = 39,
    CmdInsertDebugLabel         = 40,
    CmdClearDrawDebugLabel      = 41,
    CmdTimelineSignal           = 42,   // 时间线信号 / 等待附在回放列表的那次提交上
    CmdTimelineWait             = 43,
    CmdWriteTimestamp           = 44,
    CmdBeginQuery               = 45,
    CmdEndQuery                 = 46,
    CmdResetQueryPool           = 47,
    CmdDrawMeshTasks            = 48,
    CmdDrawMeshTasksIndirect    = 49,   // 载荷同 CmdDrawIndexedIndirect
    CmdDrawMeshTasksIndirectCount = 50, // 载荷同 CmdDrawIndexedIndirectCount
    CmdExecuteGeneratedCommands = 51,
    // 【T10】D3D12 模式 2：录制期解析好的后端细节（回放侧不再查描述符集 / 设备状态表）
    CmdResolvedBarrier          = 52,   // 一条已解析的原生资源屏障
    CmdResolvedDescriptorTables = 53,   // 已拍下的描述符表快照 GPU 句柄（按帧分版本）
    CmdResolvedRenderTargets    = 54,   // 已解析的 RTV/DSV 绑定（只读 DSV 切换等）
};

/// 一条命令的载荷：类型擦除 + **内联存储**。
/// 约定：`payloadSize <= kInlinePayloadBytes` 时参数在 `inlinePayload` 内；超过时 `payloadSize`
/// 记 0 并把实际数据放在 arena，偏移写在 `arenaOffset`（由 `EnqueueArena` 生成）。
/// L4-3 batch3/4: render pass / draw / barrier payloads (POD only, RHI-free header).
/// ClearValue 的数据量随附件数变化 ⇒ 放 arena，内联只记偏移与数量。
struct RHICommandBeginRenderPass {
    void* renderPass = nullptr;   // L4-3: render pass 随命令流携带，回放侧不依赖历史状态
    u32 colorCount = 0; u32 colorFormat = 0; u32 depthFormat = 0; u32 loadOp = 0;
    u64 clearOffset = 0; u32 clearCount = 0; u32 pad = 0;
};
struct RHICommandBeginOffscreenPass {
    void* renderPass = nullptr;   // L4-3: 同上
    void* colorImageView = nullptr; void* depthImageView = nullptr;
    u32 width = 0; u32 height = 0; u32 allowSecondary = 0;
    u64 clearOffset = 0; u32 clearCount = 0; u32 pad = 0;
};
struct RHICommandDispatch { u32 x = 0, y = 1, z = 1; u32 pad = 0; };
struct RHICommandDispatchIndirect { void* buffer = nullptr; u64 offset = 0; };
struct RHICommandCopyBuffer { void* src = nullptr; void* dst = nullptr;
    u64 size = 0; u64 srcOffset = 0; u64 dstOffset = 0; };
struct RHICommandClearDepthStencil { void* texture = nullptr; float depth = 1.0f; u32 pad = 0; };
struct RHICommandBuildTLAS { void* tlas = nullptr; void* scratch = nullptr; void* instances = nullptr;
    u32 instanceCount = 0; u32 update = 0; u32 pad = 0; };
struct RHICommandBindRTPipeline { void* rtPSO = nullptr; };
struct RHICommandCopyTextureToTexture { void* src = nullptr; void* dst = nullptr; };
struct RHICommandCopyTextureToBuffer {
    void* texture = nullptr; void* buffer = nullptr;
    u32 x = 0, y = 0, width = 0, height = 0; u32 pad = 0;
    u64 bufferOffset = 0;
};
struct RHICommandBeginOffscreenPassMRT {
    void* renderPass = nullptr;
    void* depthImageView = nullptr;
    u64   colorViewsOffset = 0;   // arena 里的 void* 数组
    u32   colorCount = 0; u32 width = 0; u32 height = 0;
    u32   allowSecondary = 0; u64 clearOffset = 0; u32 clearCount = 0; u32 pad = 0;
};
struct RHICommandDrawIndexedIndirectCount {
    void* buffer = nullptr; void* countBuffer = nullptr;
    u64 offset = 0; u64 countOffset = 0; u32 maxDrawCount = 0; u32 stride = 0;
};
struct RHICommandDrawIndexedIndirect { void* buffer = nullptr; u64 offset = 0; u32 drawCount = 0; u32 stride = 0; };
struct RHICommandDraw { u32 vertexCount = 0, instanceCount = 1, firstVertex = 0, firstInstance = 0; };
struct RHICommandPipelineBarrier { u32 srcStage = 0, dstStage = 0, srcState = 0, dstState = 0; };
/// 带纹理的屏障 / 缓冲屏障（阶段与状态都按 RHI 枚举的数值保存）
struct RHICommandPipelineBarrierTexture { void* texture = nullptr; u32 srcStage = 0, dstStage = 0, srcState = 0, dstState = 0; };
struct RHICommandBufferBarrier { void* buffer = nullptr; u32 srcState = 0, dstState = 0; };
/// 时间线信号 / 等待：围栏句柄（RHIFenceHandle）+ 值
struct RHICommandTimeline { u64 fence = 0; u64 value = 0; };
/// 查询：查询池 + 下标（ResetQueryPool 只用 pool）
struct RHICommandQuery { void* pool = nullptr; u32 index = 0; u32 pad = 0; };
/// 网格着色器直接绘制
struct RHICommandDrawMeshTasks { u32 x = 0, y = 1, z = 1; u32 pad = 0; };
/// DGC 执行描述（与 IRHICommandList::DGCExecuteDesc 逐字段对应；本头文件不依赖 RHI 类型）
struct RHICommandExecuteGeneratedCommands {
    void* indirectCommandsLayout = nullptr; void* indirectExecutionSet = nullptr;
    u64 sequencesBufferAddr = 0; u64 sequenceCountAddr = 0;
    u64 preprocessBufferAddr = 0; u64 preprocessBufferSize = 0;
    u32 maxSequenceCount = 0; u32 maxDrawCount = 0;
};
/// 【T10】一条已解析的原生资源屏障：类型数值与 IRHICommandList::ResolvedBarrierType 一致
/// （1 = 状态转换、2 = UAV、3 = 别名、4 = Discard）。before/after 是后端的状态枚举数值。
struct RHICommandResolvedBarrier {
    void* resource    = nullptr;
    u32   type        = 1;
    u32   subresource = 0xFFFFFFFFu;
    u32   before      = 0;
    u32   after       = 0;
};
/// 【T10】描述符表快照：录制线程已把描述符集内容拷进快照环，这里只带 GPU 句柄。
/// `set` 是描述符集句柄（回放侧据此取采样器表基址，也可用于诊断）。
struct RHICommandDescriptorTables {
    u32 setIndex = 0; u32 pad = 0;
    u64 set = 0; u64 srv = 0; u64 uav = 0; u64 sampler = 0;
};
/// 【T10】已解析的渲染目标绑定：RTV 数组放 arena（void* 数组），dsv 可为空
struct RHICommandResolvedRenderTargets {
    void* dsv        = nullptr;
    u64   rtvsOffset = 0;
    u32   count      = 0;
    u32   pad        = 0;
};

/// L4-3 batch2: POD only (this header is RHI-free, so no Viewport/ScissorRect types)
struct RHICommandSetViewport { float x = 0, y = 0, w = 0, h = 0, minZ = 0, maxZ = 1; };
struct RHICommandSetScissor  { i32 x = 0, y = 0; u32 w = 0, h = 0; };

struct RHICommand {
    RHICommandType type         = RHICommandType::Invalid;
    u32            payloadSize  = 0;    // 内联参数的实际字节数（0 表示走 arena）
    u64            arenaOffset  = 0;    // 走 arena 时的偏移（相对 arena 起点）
    u64            arenaSize    = 0;    // 走 arena 时的字节数
    alignas(16) u8 inlinePayload[kInlinePayloadBytes] = {};
};

// --- 各类命令的载荷（POD，全部 ≤ kInlinePayloadBytes；push constants 走 arena）---
// 【为什么参数用 void*/u64】本头文件刻意 **RHI-free**（单测直接编译它，见 Tests/TestRHICommandList.cpp），
// 所以不能出现 IRHIPipelineState / IRHIBuffer / DescriptorSetHandle 这些具体类型；执行端按约定还原。

/// `SetPipeline`：PSO 指针
struct RHICommandSetPipeline { void* pso = nullptr; };
/// `SetDescriptorSet`：set 索引 + 描述符集句柄（`DescriptorSetHandle` 是 u64）
struct RHICommandSetDescriptorSet { u32 set = 0; u32 pad = 0; u64 handle = 0; };
/// `SetPushConstants` 的**内联头**：实际字节在 arena（`ArenaRawAt(offset, size)`）
struct RHICommandSetPushConstants { u64 arenaOffset = 0; u32 size = 0; u32 offset = 0; };   // offset：推送常量内的字节偏移
/// `SetVertexBuffer`：缓冲 + 槽位
struct RHICommandSetVertexBuffer { void* buffer = nullptr; u32 slot = 0; u32 pad = 0; };
/// `SetIndexBuffer`：缓冲 + 偏移
struct RHICommandSetIndexBuffer { void* buffer = nullptr; u32 offset = 0; u32 pad = 0; };
/// `DrawIndexed`（`instanceCount > 1` 时执行端走"实例化"重载）
struct RHICommandDrawIndexed {
    u32 indexCount = 0; u32 startIndex = 0; i32 baseVertex = 0; u32 instanceCount = 1;
    u32 firstInstance = 0;   // 起始实例（D3D12 的实例号依赖它，见 kInstanceIndexSlot）
};
/// `ExecuteSecondary`：要执行的次级列表（它已由录制者线程录好）
struct RHICommandExecuteSecondary { void* secondary = nullptr; };
/// `ExecuteIndirect`：间接参数缓冲 + 绘制条数/步长
struct RHICommandExecuteIndirect {
    void* argsBuffer = nullptr; u32 maxDrawCount = 0; u32 stride = 0; u32 pad = 0;
};
/// `SetDrawDebugLabel`：内联短文本（超长截断，不足补 0）
struct RHICommandSetDebugLabel { char text[56] = {}; };

static_assert(sizeof(RHICommandSetPipeline)      <= kInlinePayloadBytes, "载荷超过内联上限");
static_assert(sizeof(RHICommandSetDescriptorSet) <= kInlinePayloadBytes, "载荷超过内联上限");
static_assert(sizeof(RHICommandSetPushConstants) <= kInlinePayloadBytes, "载荷超过内联上限");
static_assert(sizeof(RHICommandSetVertexBuffer)  <= kInlinePayloadBytes, "载荷超过内联上限");
static_assert(sizeof(RHICommandSetIndexBuffer)   <= kInlinePayloadBytes, "载荷超过内联上限");
static_assert(sizeof(RHICommandDrawIndexed)      <= kInlinePayloadBytes, "载荷超过内联上限");
static_assert(sizeof(RHICommandSetDebugLabel)    <= kInlinePayloadBytes, "载荷超过内联上限");
static_assert(sizeof(RHICommandExecuteSecondary) <= kInlinePayloadBytes, "载荷超过内联上限");
static_assert(sizeof(RHICommandExecuteIndirect)  <= kInlinePayloadBytes, "载荷超过内联上限");

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

    /// 【L4-3】把 arena 内的指针换算成相对起点偏移（`AllocArena` 只返回指针）。
    [[nodiscard]] u64 ArenaOffsetOf(const void* p) const {
        if (!p || !m_Arena) return 0u;
        const auto* b = static_cast<const u8*>(p);
        if (b < m_Arena || b >= m_Arena + m_ArenaUsed) return 0u;
        return static_cast<u64>(b - m_Arena);
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

    /// 后端辅助位：随命令流一起交接给回放端的少量标量（D3D12 用来携带
    /// "本帧用过的描述符快照环块掩码"，回放提交后据此登记围栏，见 MarkSetRingChunks）。
    /// 必须在录制线程上设置（提交前），回放线程只读。
    [[nodiscard]] u32  AuxMask() const { return m_AuxMask; }
    void SetAuxMask(u32 mask) { m_AuxMask = mask; }

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

    /// 按字节读回 arena 载荷（执行端用）：给"载荷是任意字节块"的命令（如 push constants）。
    /// 越界返回 nullptr（与 `ArenaPayloadAt` 同样的容错约定：执行端不允许对损坏的命令流 abort）。
    [[nodiscard]] const u8* ArenaRawAt(u64 offset, u64 size) const {
        if (size == 0u || offset > m_ArenaUsed || offset + size > m_ArenaUsed) return nullptr;
        return m_Arena + offset;
    }

    /// 复用本列表（下一帧重新记录）：清空命令、把 arena 已用长度归零，**保留容量与块地址**
    /// （因此已发出的指针依旧有效，只是内容不再保证）。
    void Reset() {
        m_Commands.clear();
        m_ArenaUsed = 0u;
        m_HasSubmit = false;
        m_AuxMask   = 0u;
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
    u32                     m_AuxMask       = 0;         // 后端辅助位（见 AuxMask 的说明）
};

} // namespace he::rhi
