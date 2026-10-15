// ============================================================
// TestParallelCollect.cpp — 确定性写回（§2.1）的判据
//
// 依据：docs/已实现功能/多线程渲染优化方案（并行快照构建·上传并行化·确定性写回）.md
//   · §2.1 判据：同输入**两次运行输出逐元素相同**（确定性）
//   · §4  判据：串行 / 并行结果**逐元素一致**
//
// 这个头是 RHI-free 的（只依赖 Core 的 JobSystem），所以直接编译进测试目标 —— 一旦有人把
// RHI 依赖塞进 `Render/Threading/ParallelCollect.h`，本目标会立刻编译失败。
// ============================================================
#include "doctest.h"

#include "Threading/ParallelCollect.h"

#include <vector>

using namespace he;
using namespace he::render;

namespace {

/// 被测函数用 `JobSystem::Instance()`（单例）：测试里必须有实例，否则解引用空指针
struct JobSystemGuard {
    JobSystemGuard()  { JobSystem::Initialize(); }
    ~JobSystemGuard() { JobSystem::Shutdown(); }
};

enum class HitPattern { None, All, Alternate, ChunkHead, Dense, Sparse, Random };

std::vector<u8> MakeHit(u32 n, HitPattern p) {
    std::vector<u8> hit(n, 0u);
    u32 seed = 12345u;
    for (u32 i = 0; i < n; ++i) {
        bool on = false;
        switch (p) {
        case HitPattern::None:      on = false; break;
        case HitPattern::All:       on = true;  break;
        case HitPattern::Alternate: on = (i % 2u) == 0u; break;
        case HitPattern::ChunkHead: on = (i % kCollectChunkSize) == 0u; break;   // 块边界
        case HitPattern::Dense:     on = (i % 8u) != 7u; break;
        case HitPattern::Sparse:    on = (i % 997u) == 3u; break;
        case HitPattern::Random:                                            // 固定种子 LCG
            seed = seed * 1664525u + 1013904223u;
            on = ((seed >> 16) & 0x3u) == 0u;
            break;
        }
        hit[i] = on ? 1u : 0u;
    }
    return hit;
}

/// 串行参考（朴素语义）：命中项按原下标升序
std::vector<u32> NaiveCompact(const std::vector<u8>& hit) {
    std::vector<u32> out;
    for (u32 i = 0; i < static_cast<u32>(hit.size()); ++i)
        if (hit[i]) out.push_back(i);
    return out;
}

} // namespace

TEST_CASE("CompactHits：串行实现与朴素语义一致") {
    JobSystemGuard guard;
    for (u32 n : {0u, 1u, 7u, 64u, 65u, 1000u}) {
        for (auto p : {HitPattern::None, HitPattern::All, HitPattern::Alternate,
                       HitPattern::ChunkHead, HitPattern::Sparse, HitPattern::Random}) {
            const auto hit = MakeHit(n, p);
            std::vector<u32> out(hit.size(), 0xFFFFFFFFu);
            const u32 count = CompactHitsSerial(hit, out);
            const auto expect = NaiveCompact(hit);
            CHECK(count == static_cast<u32>(expect.size()));
            CHECK(std::vector<u32>(out.begin(), out.begin() + count) == expect);
        }
    }
}

TEST_CASE("CompactHits：并行与串行逐元素一致（多规模 × 多图案）") {
    JobSystemGuard guard;
    // 规模覆盖：低于/正好/高于并行阈值，以及大量非整块
    for (u32 n : {0u, 1u, 63u, 64u, 65u, 1023u, 1024u, 1025u, 4096u, 5000u, 10007u}) {
        for (auto p : {HitPattern::None, HitPattern::All, HitPattern::Alternate,
                       HitPattern::ChunkHead, HitPattern::Dense, HitPattern::Sparse,
                       HitPattern::Random}) {
            const auto hit = MakeHit(n, p);

            std::vector<u32> serial(hit.size(), 0xFFFFFFFFu);
            const u32 serialCount = CompactHitsSerial(hit, serial);

            std::vector<u32> parallel(hit.size(), 0xFFFFFFFFu);
            const u32 parallelCount = CompactHits(hit, parallel);

            INFO("n=", n, " pattern=", static_cast<int>(p));
            REQUIRE(parallelCount == serialCount);
            // 逐元素一致（只比较有效区间；尾部哨兵也必须没被写过）
            CHECK(std::equal(parallel.begin(), parallel.begin() + parallelCount, serial.begin()));
            for (usize i = parallelCount; i < parallel.size(); ++i)
                CHECK(parallel[i] == 0xFFFFFFFFu);
        }
    }
}

TEST_CASE("CompactHits：同输入多次运行输出逐元素相同（确定性判据）") {
    JobSystemGuard guard;
    const u32 n = 8192u;
    const auto hit = MakeHit(n, HitPattern::Random);

    std::vector<u32> first(hit.size(), 0u);
    const u32 firstCount = CompactHits(hit, first);

    // 重复跑：结果必须逐元素相同（顺序不随线程调度变化）
    for (int rep = 0; rep < 8; ++rep) {
        std::vector<u32> again(hit.size(), 0u);
        const u32 againCount = CompactHits(hit, again);
        REQUIRE(againCount == firstCount);
        CHECK(again == first);
    }

    // 不同 chunkSize 下也必须是同一序列（分块只影响并行度，不影响结果）
    for (u32 cs : {32u, 64u, 128u, 1000u}) {
        std::vector<u32> out(hit.size(), 0u);
        const u32 count = CompactHits(hit, out, cs);
        REQUIRE(count == firstCount);
        CHECK(out == first);
    }
}

TEST_CASE("CompactHits：边界情形") {
    JobSystemGuard guard;
    SUBCASE("空输入") {
        std::vector<u8> hit;
        std::vector<u32> out;
        CHECK(CompactHits(hit, out) == 0u);
        CHECK(CompactHitsSerial(hit, out) == 0u);
    }
    SUBCASE("单元素：命中与不命中") {
        std::vector<u8> on{1u}, off{0u};
        std::vector<u32> out(1u, 0u);
        CHECK(CompactHits(on, out) == 1u);   CHECK(out[0] == 0u);
        out[0] = 0u;
        CHECK(CompactHits(off, out) == 0u);  CHECK(out[0] == 0u);
    }
    SUBCASE("只在最后一块命中（并行路径）") {
        const u32 n = 4096u;
        std::vector<u8> hit(n, 0u);
        hit[n - 1u] = 1u;
        std::vector<u32> out(n, 0u);
        CHECK(CompactHits(hit, out) == 1u);
        CHECK(out[0] == n - 1u);
    }
    SUBCASE("输出顺序恒为下标升序") {
        auto hit = MakeHit(3000u, HitPattern::Random);
        std::vector<u32> out(hit.size(), 0u);
        const u32 count = CompactHits(hit, out);
        for (u32 i = 1; i < count; ++i) CHECK(out[i - 1u] < out[i]);
    }
}
