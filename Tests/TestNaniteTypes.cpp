// ============================================================
// Tests/TestNaniteTypes.cpp — Nanite 纯数据类型与 objectIndex 分区契约单测（§14.8 任务 5）
//
// 为什么能脱离 RHI：分区契约与实例槽分配器全部下沉到 `Nanite/NaniteTypes.h`
//   （只依赖 Core/Types.h），故本文件只需把 `Engine/Render` 加入 include 路径，
//   **无需链接 HugEngineRender**（链接它会连带拉入 RHI/Vulkan 与 slangc 生成的 SPV 头）。
//
// 覆盖范围：
//   1. 分区表数值与段起止（普通段 / Nanite 段 / 哨兵 / 总容量）
//   2. 段边界逐点：0、1023、1024、1024+容量-1、1024+容量、0xFFFFFFFF
//   3. 局部索引 ↔ 全局 objectIndex 的往返（含首尾，且全空间逐点往返）
//   4. 哨兵不与任何合法索引冲突
//   5. 硬约束：Nanite 段的索引**不被普通段解码接受**（混排场景的正确性前提）
//   6. MRT7（gb_lightmapkey）页号在 binary16 精确整数范围内
//   7. 实例槽分配器：恰好容量次成功、第 容量+1 次返回哨兵、回收后可复用
//   8. 任务 3/4 的共享 POD 尺寸与偏移不漂移
// ============================================================

#include "doctest.h"

#include "Nanite/NaniteTypes.h"   // 分区契约 + 实例槽分配器 + 任务 3/4 的 POD（RHI-free）

#include <cstddef>   // offsetof
#include <vector>

using namespace he;
using namespace he::render;

// ============================================================
// 1. 分区表数值与段起止
// ============================================================
TEST_CASE("NaniteTypes: objectIndex 分区表数值与段起止") {
    // 普通段从 0 起（SceneRenderer/MeshBatcher/GPUScene 的枚举顺序契约依赖它）
    CHECK(kNormalObjectIndexBegin == 0u);
    // 普通段容量必须等于 kGPUMaxObjects = MAX_OBJECTS（GPUObjectData 缓冲上限）
    CHECK(kNormalObjectIndexCapacity == 1024u);
    CHECK(kNaniteObjectIndexBegin == 1024u);
    CHECK(kNaniteObjectIndexCapacity == 1024u);
    CHECK(kObjectIndexTotalCapacity == 2048u);

    // 两段首尾相接：不重叠、也没有空洞
    CHECK(kNaniteObjectIndexBegin == kNormalObjectIndexBegin + kNormalObjectIndexCapacity);
    CHECK(kObjectIndexTotalCapacity == kNaniteObjectIndexBegin + kNaniteObjectIndexCapacity);

    // 哨兵在合法范围之外，且是 0xFFFFFFFF
    CHECK(kInvalidObjectIndex == 0xFFFFFFFFu);
    CHECK(kInvalidObjectIndex >= kObjectIndexTotalCapacity);

    // 整个 ID 空间必须能被 RGBA16_FLOAT（binary16）精确表示：2^11 = 2048
    // （任务 5 把 Nanite 段容量从任务 1 预置的 16384 收到 1024，就是因为这条）
    CHECK(kLightmapKeyExactObjectIndexLimit == 2048u);
    CHECK(kObjectIndexTotalCapacity <= kLightmapKeyExactObjectIndexLimit);
}

// ============================================================
// 2. 段边界逐点判定
// ============================================================
TEST_CASE("NaniteTypes: 段边界逐点判定（0/1023/1024/尾/尾+1/0xFFFFFFFF）") {
    struct Case {
        u32                idx;
        ObjectIndexSegment seg;
        bool               valid;
        bool               normal;
        bool               nanite;
    };
    const Case cases[] = {
        // 普通段首 / 尾
        { 0u,                                              ObjectIndexSegment::Normal,  true,  true,  false },
        { kNormalObjectIndexCapacity - 1u,                 ObjectIndexSegment::Normal,  true,  true,  false },
        // Nanite 段首 / 尾（= 1024 / 1024+容量-1）
        { kNaniteObjectIndexBegin,                         ObjectIndexSegment::Nanite,  true,  false, true  },
        { kNaniteObjectIndexBegin + kNaniteObjectIndexCapacity - 1u,
                                                           ObjectIndexSegment::Nanite,  true,  false, true  },
        // Nanite 段尾 +1（= 1024+容量）：越界
        { kNaniteObjectIndexBegin + kNaniteObjectIndexCapacity,
                                                           ObjectIndexSegment::Invalid, false, false, false },
        // 哨兵
        { kInvalidObjectIndex,                             ObjectIndexSegment::Invalid, false, false, false },
    };

    for (const Case& c : cases) {
        CHECK(IsValidObjectIndex(c.idx) == c.valid);
        CHECK(IsNormalObjectIndex(c.idx) == c.normal);
        CHECK(IsNaniteObjectIndex(c.idx) == c.nanite);
        CHECK(ClassifyObjectIndex(c.idx) == c.seg);
        CHECK(IsInvalidObjectIndex(c.idx) == (c.idx == kInvalidObjectIndex));
    }
}

// ============================================================
// 3. 局部索引 ↔ 全局 objectIndex 的往返
// ============================================================
TEST_CASE("NaniteTypes: 局部索引与全局 objectIndex 往返（含首尾）") {
    // 普通段：本地下标 == 全局索引（段起点为 0）
    CHECK(NormalObjectIndexFromLocal(0u) == 0u);
    CHECK(NormalObjectIndexFromLocal(kNormalObjectIndexCapacity - 1u) == kNormalObjectIndexCapacity - 1u);
    CHECK(NormalLocalIndex(0u) == 0u);
    CHECK(NormalLocalIndex(kNormalObjectIndexCapacity - 1u) == kNormalObjectIndexCapacity - 1u);

    // Nanite 段：全局 = 1024 + 本地下标
    CHECK(NaniteObjectIndexFromLocal(0u) == kNaniteObjectIndexBegin);
    CHECK(NaniteObjectIndexFromLocal(kNaniteObjectIndexCapacity - 1u) == kObjectIndexTotalCapacity - 1u);
    CHECK(NaniteLocalIndex(kNaniteObjectIndexBegin) == 0u);
    CHECK(NaniteLocalIndex(kObjectIndexTotalCapacity - 1u) == kNaniteObjectIndexCapacity - 1u);

    // 全空间逐点往返：TrySplit -> ObjectIndexFromLocal 必须回到原值
    for (u32 idx = 0; idx < kObjectIndexTotalCapacity; ++idx) {
        ObjectIndexSegment seg   = ObjectIndexSegment::Invalid;
        u32                local = 0xDEADBEEFu;
        REQUIRE(TrySplitObjectIndex(idx, seg, local));
        CHECK(ObjectIndexFromLocal(seg, local) == idx);
    }

    // 段外（尾 +1）的本地下标必须被拒：返回哨兵，绝不能"绕回来"落到另一段
    CHECK(ObjectIndexFromLocal(ObjectIndexSegment::Normal, kNormalObjectIndexCapacity)
          == kInvalidObjectIndex);
    CHECK(ObjectIndexFromLocal(ObjectIndexSegment::Nanite, kNaniteObjectIndexCapacity)
          == kInvalidObjectIndex);
    CHECK(ObjectIndexFromLocal(ObjectIndexSegment::Invalid, 0u) == kInvalidObjectIndex);

    // 落不进任何段的索引：TrySplit 返回 false，且**不改写**出参
    ObjectIndexSegment seg   = ObjectIndexSegment::Normal;
    u32                local = 7u;
    CHECK_FALSE(TrySplitObjectIndex(kObjectIndexTotalCapacity, seg, local));
    CHECK(seg == ObjectIndexSegment::Normal);
    CHECK(local == 7u);
    CHECK_FALSE(TrySplitObjectIndex(kInvalidObjectIndex, seg, local));
    CHECK(seg == ObjectIndexSegment::Normal);
    CHECK(local == 7u);
}

// ============================================================
// 4. 哨兵不与任何合法索引冲突
// ============================================================
TEST_CASE("NaniteTypes: 哨兵不与任何合法索引冲突") {
    CHECK(kInvalidObjectIndex >= kObjectIndexTotalCapacity);   // 落在两段之外
    CHECK_FALSE(IsValidObjectIndex(kInvalidObjectIndex));
    CHECK_FALSE(IsNormalObjectIndex(kInvalidObjectIndex));
    CHECK_FALSE(IsNaniteObjectIndex(kInvalidObjectIndex));
    CHECK(IsInvalidObjectIndex(kInvalidObjectIndex));

    CHECK_FALSE(IsInvalidObjectIndex(0u));
    CHECK_FALSE(IsInvalidObjectIndex(kNaniteObjectIndexBegin));
    CHECK_FALSE(IsInvalidObjectIndex(kObjectIndexTotalCapacity - 1u));
    // 哨兵也不是合法页号（写进 MRT7 只会得到 NaN）
    CHECK_FALSE(IsLightmapKeyPageExact(kInvalidObjectIndex));

    // 逐点：哨兵不等于任何一个合法索引
    for (u32 idx = 0; idx < kObjectIndexTotalCapacity; ++idx)
        CHECK(idx != kInvalidObjectIndex);
}

// ============================================================
// 5. 硬约束：Nanite 段索引不被普通段解码接受
//
// 这就是"混排场景下 gb_lightmapkey 解析正确、无越界"的 C++ 侧前提：
// 普通段解码器（`u_Objects[idx]` / 页号 < 1024）必须先过 IsNormalObjectIndex，
// Nanite 段（1024 起）一个也不能通过。
// ============================================================
TEST_CASE("NaniteTypes: Nanite 段索引不被普通段解码接受") {
    // 模拟一个"按普通段容量解码"的函数（等价于 `u_Objects[idx]` / `page < 1024`）
    const auto DecodesAsNormalSegment = [](u32 objectIndex) {
        return IsNormalObjectIndex(objectIndex);
    };

    // 普通段整段可解
    CHECK(DecodesAsNormalSegment(0u));
    CHECK(DecodesAsNormalSegment(kNormalObjectIndexCapacity - 1u));

    // Nanite 段整段都不可用普通段解码（首、第二个、尾、尾 +1、哨兵）
    CHECK_FALSE(DecodesAsNormalSegment(kNaniteObjectIndexBegin));
    CHECK_FALSE(DecodesAsNormalSegment(kNaniteObjectIndexBegin + 1u));
    CHECK_FALSE(DecodesAsNormalSegment(kObjectIndexTotalCapacity - 1u));
    CHECK_FALSE(DecodesAsNormalSegment(kObjectIndexTotalCapacity));
    CHECK_FALSE(DecodesAsNormalSegment(kInvalidObjectIndex));

    // 逐点：Nanite 段的每一个索引都只能经本地下标索引模块自持缓冲
    for (u32 local = 0; local < kNaniteObjectIndexCapacity; ++local) {
        const u32 idx = NaniteObjectIndexFromLocal(local);
        CHECK_FALSE(IsNormalObjectIndex(idx));
        CHECK(IsNaniteObjectIndex(idx));
        CHECK(NaniteLocalIndex(idx) == local);
    }
}

// ============================================================
// 6. MRT7（gb_lightmapkey）页号在 binary16 精确整数范围内
// ============================================================
TEST_CASE("NaniteTypes: MRT7 页号在 binary16 精确整数范围内") {
    // 合法索引（0..2047）全部可被 RGBA16_FLOAT 的 .z 精确表示
    for (u32 idx = 0; idx < kObjectIndexTotalCapacity; ++idx)
        CHECK(IsLightmapKeyPageExact(idx));

    // 2^11 = 2048 本身也可精确表示（它是二的幂）
    CHECK(IsLightmapKeyPageExact(kObjectIndexTotalCapacity));

    // 2049 = 2^11 + 1 落在 [2048,4096) —— binary16 在该区间的间距是 2，会被量化成 2048。
    // 这一条正是任务 1 的 16384 被收到 1024 的原因：若 Nanite 段开到 17408，
    // 页号 2049 在 GBuffer 里读回来就是 2048，"解析正确"不成立。
    CHECK_FALSE(IsLightmapKeyPageExact(kObjectIndexTotalCapacity + 1u));
}

// ============================================================
// 7. 实例槽分配器：容量、哨兵、回收复用
// ============================================================
TEST_CASE("NaniteTypes: 实例槽分配器容量与哨兵") {
    NaniteInstanceSlotAllocator alloc;
    CHECK(NaniteInstanceSlotAllocator::Capacity() == kNaniteObjectIndexCapacity);
    CHECK(alloc.AllocatedCount() == 0u);
    CHECK(alloc.FreeCount() == NaniteInstanceSlotAllocator::Capacity());

    // 连续分配恰好容量次：全部成功、全部落在 Nanite 段、互不相同
    std::vector<u32> got;
    got.reserve(NaniteInstanceSlotAllocator::Capacity());
    for (u32 i = 0; i < NaniteInstanceSlotAllocator::Capacity(); ++i) {
        const u32 idx = alloc.Allocate();
        REQUIRE(idx != kInvalidObjectIndex);
        CHECK(IsNaniteObjectIndex(idx));
        got.push_back(idx);
    }
    CHECK(alloc.AllocatedCount() == NaniteInstanceSlotAllocator::Capacity());
    CHECK(alloc.FreeCount() == 0u);

    // 确定性：最小空闲优先 ⇒ 分配序列恰好是 Nanite 段本地下标 0..容量-1
    for (u32 i = 0; i < (u32)got.size(); ++i) {
        CHECK(NaniteLocalIndex(got[i]) == i);
        CHECK(alloc.IsAllocated(got[i]));
    }

    // 第 容量 + 1 次必须返回哨兵（不静默越界、不崩）
    CHECK(alloc.Allocate() == kInvalidObjectIndex);
    CHECK(alloc.AllocatedCount() == NaniteInstanceSlotAllocator::Capacity());
}

TEST_CASE("NaniteTypes: 实例槽分配器回收后可复用") {
    NaniteInstanceSlotAllocator alloc;
    const u32 a = alloc.Allocate();
    const u32 b = alloc.Allocate();
    const u32 c = alloc.Allocate();
    REQUIRE(a == NaniteObjectIndexFromLocal(0u));
    REQUIRE(b == NaniteObjectIndexFromLocal(1u));
    REQUIRE(c == NaniteObjectIndexFromLocal(2u));
    CHECK(alloc.AllocatedCount() == 3u);

    // 回收 a：最小空闲优先 ⇒ 下一次分配立刻拿回 a（确定性复用）
    CHECK(alloc.Free(a));
    CHECK_FALSE(alloc.IsAllocated(a));
    CHECK(alloc.AllocatedCount() == 2u);
    CHECK(alloc.Allocate() == a);
    CHECK(alloc.IsAllocated(a));

    // 重复回收 / 回收普通段 / 回收哨兵 / 回收越界：一律拒绝
    CHECK(alloc.Free(b));
    CHECK_FALSE(alloc.Free(b));                                       // 重复回收
    CHECK_FALSE(alloc.Free(0u));                                      // 普通段首
    CHECK_FALSE(alloc.Free(kNormalObjectIndexCapacity - 1u));         // 普通段尾
    CHECK_FALSE(alloc.Free(kInvalidObjectIndex));                     // 哨兵
    CHECK_FALSE(alloc.Free(kObjectIndexTotalCapacity));               // 越界（尾 +1）

    // Reset 之后可以再分配满容量，第 容量+1 次仍然是哨兵
    alloc.Reset();
    CHECK(alloc.AllocatedCount() == 0u);
    CHECK(alloc.FreeCount() == NaniteInstanceSlotAllocator::Capacity());
    for (u32 i = 0; i < NaniteInstanceSlotAllocator::Capacity(); ++i)
        CHECK(alloc.Allocate() != kInvalidObjectIndex);
    CHECK(alloc.Allocate() == kInvalidObjectIndex);
}

// ============================================================
// 8. 任务 3/4 的共享 POD：尺寸与偏移不漂移
// ============================================================
TEST_CASE("NaniteTypes: 任务 3/4 共享 POD 的尺寸与偏移") {
    // NaniteIndirectCommand 必须与 VkDrawIndexedIndirectCommand 二进制兼容（20B / 步长）
    static_assert(sizeof(NaniteIndirectCommand) == 20, "间接命令必须是 20 字节");
    CHECK(sizeof(NaniteIndirectCommand) == 20u);
    CHECK(offsetof(NaniteIndirectCommand, indexCount)    == 0u);
    CHECK(offsetof(NaniteIndirectCommand, instanceCount) == 4u);
    CHECK(offsetof(NaniteIndirectCommand, firstIndex)    == 8u);
    CHECK(offsetof(NaniteIndirectCommand, vertexOffset)  == 12u);
    CHECK(offsetof(NaniteIndirectCommand, firstInstance) == 16u);

    // 假簇条目（Nanite_Cull.comp.slang 的 FakeCluster 镜像）
    static_assert(sizeof(NaniteFakeCluster) == 16, "假簇条目必须 16 字节");
    CHECK(sizeof(NaniteFakeCluster) == 16u);

    // 计数缓冲 = 单个 u32（GPU InterlockedAdd 的目标）
    CHECK(kNaniteCountBufferU32 == 1u);
    CHECK(kNaniteCountBufferSize == sizeof(u32));

    // 任务 3 的假数据常量：1 个三角形 = 3 个索引；绘制端目标 1×1（不是 GBuffer 附件）
    CHECK(kNaniteFakeClusterIndexCount == 3u);
    CHECK(kNaniteRasterTargetSize == 1u);
    CHECK(kNaniteMaxFakeClusters > 0u);
}
