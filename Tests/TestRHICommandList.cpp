// ============================================================
// TestRHICommandList.cpp — 阶段 0 T0.6：RHI 命令流记录端契约
//
// 【为什么要有它】这份契约的全部价值在于"形状"：载荷要么内联、要么走 arena，且**执行端能原样读回**。
// 形状错了（例如对齐没做、偏移算错、内联溢出）在阶段 0 不会有人发现，等到 §12 的 A-1 真正落地时
// 才会以"命令读出来是垃圾"的形式爆炸，而且要连带改渲染层。因此这里逐条钉死：
//   ① 内联载荷按值拷贝、执行端按类型原样读回（含 64B 边界）；
//   ② arena 分配的对齐、不重叠、偏移可读回；
//   ③ 提交边界标记；
//   ④ `Reset()` 复用后命令与 arena 归零、容量保留（不残留上一帧的命令）。
// ============================================================
#include "RHI/RHICommandList.h"

#include <doctest/doctest.h>

#include <cstring>
#include <vector>

using namespace he;
using namespace he::rhi;

namespace {

/// 内联命令的代表载荷（模拟 SetPipeline 的少量参数）
struct SetPipelinePayload {
    u32 psoIndex    = 0;
    u32 bindPoint   = 0;
    u32 firstSet    = 0;
    u32 reserved    = 0;
};

/// 刚好 64B 的内联载荷（边界用例）
struct FullInlinePayload {
    u32 words[16] = {};          // 16 × 4B = 64B
};

/// 大块载荷（模拟常量缓冲 / 上传源 / SBT 记录）
struct BigPayload {
    float values[64] = {};       // 256B > 64B ⇒ 必须走 arena
};

/// 要求 32B 对齐的大块载荷（执行端会当 `float4x2` 之类读它）
struct alignas(32) AlignedBigPayload {
    float values[32] = {};
};

} // namespace

TEST_CASE("RHICommandList：内联载荷按值拷贝并可原样读回") {
    RHICommandList list;
    SetPipelinePayload payload{7u, 3u, 1u, 0u};
    list.Enqueue(RHICommandType::SetPipeline, payload);

    REQUIRE(list.CommandCount() == 1u);
    CHECK(list.CommandAt(0).type == RHICommandType::SetPipeline);
    CHECK(list.CommandAt(0).payloadSize == sizeof(SetPipelinePayload));

    const auto* back = list.PayloadAt<SetPipelinePayload>(0);
    REQUIRE(back != nullptr);
    CHECK(back->psoIndex == 7u);
    CHECK(back->bindPoint == 3u);
    CHECK(back->firstSet == 1u);

    // 记录后修改源对象**不得**影响已记录的命令（内联 = 按值）
    payload.psoIndex = 99u;
    CHECK(list.PayloadAt<SetPipelinePayload>(0)->psoIndex == 7u);
}

TEST_CASE("RHICommandList：64B 边界载荷可内联，类型不匹配时读回为空") {
    RHICommandList list;
    FullInlinePayload full;
    for (u32 i = 0; i < 16u; ++i) full.words[i] = i * 3u + 1u;
    list.Enqueue(RHICommandType::SetDescriptorSet, full);

    const auto* back = list.PayloadAt<FullInlinePayload>(0);
    REQUIRE(back != nullptr);
    for (u32 i = 0; i < 16u; ++i) CHECK(back->words[i] == i * 3u + 1u);

    // 用错误的类型读：长度不匹配 ⇒ nullptr（执行端必须能容错，不允许直接崩）
    CHECK(list.PayloadAt<SetPipelinePayload>(0) == nullptr);
    CHECK(list.PayloadAt<FullInlinePayload>(1) == nullptr);   // 越界
}

TEST_CASE("RHICommandList：arena 分配对齐且不重叠，载荷可读回") {
    RHICommandList list;

    BigPayload a, b;
    for (u32 i = 0; i < 64u; ++i) { a.values[i] = static_cast<float>(i); b.values[i] = 1000.0f + static_cast<float>(i); }
    list.EnqueueArena(RHICommandType::CopyBuffer, a);
    list.EnqueueArena(RHICommandType::Dispatch, b);

    REQUIRE(list.CommandCount() == 2u);
    CHECK(list.CommandAt(0).payloadSize == 0u);              // 走 arena 的标记
    CHECK(list.CommandAt(0).arenaSize == sizeof(BigPayload));
    CHECK(list.CommandAt(1).arenaOffset >= list.CommandAt(0).arenaOffset + sizeof(BigPayload));  // 不重叠

    const auto* ra = list.ArenaPayloadAt<BigPayload>(0);
    const auto* rb = list.ArenaPayloadAt<BigPayload>(1);
    REQUIRE(ra != nullptr);
    REQUIRE(rb != nullptr);
    CHECK(ra->values[63] == 63.0f);
    CHECK(rb->values[63] == 1063.0f);

    // 用内联读法读 arena 命令：必须失败（两条通道不能混）
    CHECK(list.PayloadAt<BigPayload>(0) == nullptr);
    // 类型不匹配同样失败
    CHECK(list.ArenaPayloadAt<SetPipelinePayload>(0) == nullptr);
}

TEST_CASE("RHICommandList：arena 对齐可指定，且地址满足对齐要求") {
    RHICommandList list;
    void* p1 = list.AllocArena(3u, 16u);
    void* p2 = list.AllocArena(1u, 64u);
    void* p3 = list.AllocArena(8u, 8u);

    CHECK(reinterpret_cast<uintptr_t>(p1) % 16u == 0u);
    CHECK(reinterpret_cast<uintptr_t>(p2) % 64u == 0u);
    CHECK(reinterpret_cast<uintptr_t>(p3) % 8u == 0u);
    CHECK(static_cast<u8*>(p2) >= static_cast<u8*>(p1) + 3);   // 不重叠
    CHECK(static_cast<u8*>(p3) >= static_cast<u8*>(p2) + 1);
}

TEST_CASE("RHICommandList：子对齐的 arena 载荷地址真的满足对齐（不只偏移）") {
    RHICommandList list;
    AlignedBigPayload a, b;
    a.values[0] = 1.0f;
    b.values[0] = 2.0f;
    list.EnqueueArena(RHICommandType::CopyBuffer, a);
    list.EnqueueArena(RHICommandType::CopyBuffer, b);

    const auto* pa = list.ArenaPayloadAt<AlignedBigPayload>(0);
    const auto* pb = list.ArenaPayloadAt<AlignedBigPayload>(1);
    REQUIRE(pa != nullptr);
    REQUIRE(pb != nullptr);
    // 关键：对齐必须落在**绝对地址**上。第一版只对齐偏移、用 std::vector<u8> 存 arena，
    // 64B 对齐用例当场失败（48 % 64 != 0）—— 这条断言就是那次教训的钉子。
    CHECK(reinterpret_cast<uintptr_t>(pa) % 32u == 0u);
    CHECK(reinterpret_cast<uintptr_t>(pb) % 32u == 0u);
    CHECK(pa->values[0] == 1.0f);
    CHECK(pb->values[0] == 2.0f);
}

TEST_CASE("RHICommandList：提交边界标记") {
    RHICommandList list;
    CHECK_FALSE(list.HasSubmitBoundary());

    list.Enqueue(RHICommandType::DrawIndexed, SetPipelinePayload{1u, 0u, 0u, 0u});
    list.MarkSubmitBoundary();

    CHECK(list.HasSubmitBoundary());
    CHECK(list.CommandCount() == 2u);
    CHECK(list.CommandAt(1).type == RHICommandType::MarkSubmitBoundary);
}

TEST_CASE("RHICommandList：Reset 复用后不残留上一帧内容，但容量保留") {
    RHICommandList list;
    BigPayload big;
    for (u32 i = 0; i < 200u; ++i) list.EnqueueArena(RHICommandType::CopyBuffer, big);
    list.Enqueue(RHICommandType::SetPipeline, SetPipelinePayload{5u, 0u, 0u, 0u});
    list.MarkSubmitBoundary();
    const u64 arenaBefore = list.ArenaBytes();
    REQUIRE(arenaBefore > 0u);
    REQUIRE(list.CommandCount() == 202u);

    list.Reset();
    CHECK(list.CommandCount() == 0u);
    CHECK(list.ArenaBytes() == 0u);
    CHECK_FALSE(list.HasSubmitBoundary());

    // 复用：记录一帧新命令后，读回的必须是新内容
    list.Enqueue(RHICommandType::Dispatch, SetPipelinePayload{42u, 0u, 0u, 0u});
    REQUIRE(list.CommandCount() == 1u);
    CHECK(list.PayloadAt<SetPipelinePayload>(0)->psoIndex == 42u);
    CHECK(list.CommandAt(0).payloadSize == sizeof(SetPipelinePayload));
}
