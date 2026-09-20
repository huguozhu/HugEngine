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
//   9. 任务 7：`.nanite` 文件头逐字段（魔数/版本/计数/flags/bbox/保留区）与 96B 尺寸
//  10. 任务 7：簇记录（64B）、cone 轴角字段（16B）的尺寸与偏移
//  11. 任务 7：顶点记录（16B）与量化偏置的落点（quantBias）
//  12. 任务 7：量化偏置往返（编码端 +512 与 §8.4 解码互逆）
//  13. 任务 7：三角形索引编码（3×u16 进 u32[2]）的位边界与簇内下标语义边界
//  14. 任务 7：cone 数据解码（单位轴 / cos 半角 / 无锥哨兵）
//  15. 任务 7：段表推导（计数 → 各段 offset/size、16B 对齐、总长）
//  16. 任务 7：校验函数的正例与反例（空指针/截断/魔数错/版本错/索引数错/越界/尾部多余）
// ============================================================

#include "doctest.h"

#include "Nanite/NaniteTypes.h"   // 分区契约 + 实例槽分配器 + 任务 3/4 的 POD（RHI-free）

#include <cmath>     // std::fabs / std::acos（cone 解码与量化误差）
#include <cstddef>   // offsetof
#include <cstring>   // memcpy（把头部写进测试缓冲）
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

// ============================================================
// 任务 7 的单测工具：按头部计数造一个"最小合法"的 `.nanite` 缓冲（各段 0 填充）
// ============================================================
namespace {

/// 返回推导出的段表（顺带把头部 memcpy 到缓冲头部）；推导失败时返回的段表 totalBytes == 0
NaniteFileLayout BuildNaniteBuffer(const NaniteFileHeader& header, std::vector<u8>& buffer,
                                   usize extraBytes = 0u) {
    NaniteFileLayout layout{};
    if (TryBuildNaniteFileLayout(header, layout) != NaniteFileError::None) return NaniteFileLayout{};
    buffer.assign(layout.totalBytes + extraBytes, 0u);
    std::memcpy(buffer.data(), &header, sizeof(header));
    return layout;
}

} // namespace

// ============================================================
// 9. 任务 7：`.nanite` 文件头字段与尺寸（裁决 #8：96B 而非 128B）
// ============================================================
TEST_CASE("NaniteTypes: .nanite 文件头字段与尺寸（任务 7 定稿 96B）") {
    // 编译期钉子（与 NaniteTypes.h 内的一致，这里再钉一次）
    static_assert(sizeof(NaniteFileHeader) == 96, "文件头必须 96B（不是 128B）");
    static_assert(alignof(NaniteFileHeader) == 16, "文件头必须 16B 对齐");

    CHECK(sizeof(NaniteFileHeader) == 96u);
    CHECK(alignof(NaniteFileHeader) == 16u);
    CHECK(kNaniteFileHeaderBytes == 96u);
    CHECK(kNaniteFileHeaderBytes % kNaniteFileAlignment == 0u);   // 96 = 6×16 天然对齐
    CHECK(kNaniteFileHeaderReservedU32 == 8u);

    // 逐字段偏移（§8.3 的表格）
    CHECK(offsetof(NaniteFileHeader, magic)         == 0u);
    CHECK(offsetof(NaniteFileHeader, version)       == 8u);
    CHECK(offsetof(NaniteFileHeader, clusterCount)  == 12u);
    CHECK(offsetof(NaniteFileHeader, vertexCount)   == 16u);
    CHECK(offsetof(NaniteFileHeader, indexCount)    == 20u);
    CHECK(offsetof(NaniteFileHeader, materialCount) == 24u);
    CHECK(offsetof(NaniteFileHeader, lodLevelCount) == 28u);
    CHECK(offsetof(NaniteFileHeader, flags)         == 32u);
    CHECK(offsetof(NaniteFileHeader, bboxMin)       == 36u);
    CHECK(offsetof(NaniteFileHeader, bboxMax)       == 48u);
    CHECK(offsetof(NaniteFileHeader, maxLODError)   == 60u);
    CHECK(offsetof(NaniteFileHeader, _reserved)     == 64u);

    // 逐字段默认值
    const NaniteFileHeader header{};
    CHECK(kNaniteFileVersion == 1u);
    CHECK(header.version == 1u);

    // 魔数：恰好 8 字节 "NANITE01"，逐字节相等且**不带 NUL 结尾**
    const char expectedMagic[8] = { 'N', 'A', 'N', 'I', 'T', 'E', '0', '1' };
    for (u32 i = 0; i < 8u; ++i) {
        CHECK(header.magic[i] == expectedMagic[i]);
        CHECK(header.magic[i] == kNaniteFileMagic[i]);
        CHECK(header.magic[i] != '\0');
    }

    CHECK(header.clusterCount  == 0u);
    CHECK(header.vertexCount   == 0u);
    CHECK(header.indexCount    == 0u);
    CHECK(header.materialCount == 0u);
    CHECK(header.lodLevelCount == 0u);
    CHECK(header.flags         == 0u);
    CHECK(kNaniteFileFlagHasDAG == 1u);                          // flags 的 bit0 = hasDAG
    CHECK((kNaniteFileFlagHasDAG & 0x1u) != 0u);
    CHECK(header.bboxMin[0] == 0.0f);
    CHECK(header.bboxMin[1] == 0.0f);
    CHECK(header.bboxMin[2] == 0.0f);
    CHECK(header.bboxMax[0] == 0.0f);
    CHECK(header.bboxMax[1] == 0.0f);
    CHECK(header.bboxMax[2] == 0.0f);
    CHECK(header.maxLODError == 0.0f);
    for (u32 i = 0; i < kNaniteFileHeaderReservedU32; ++i)
        CHECK(header._reserved[i] == 0u);                        // 保留区写 0（段偏移不落盘）
}

// ============================================================
// 10. 任务 7：簇记录（64B）与 cone 轴角字段（16B）
// ============================================================
TEST_CASE("NaniteTypes: 簇记录与 cone 轴角字段（任务 7 定稿 64B / 16B）") {
    static_assert(sizeof(NaniteClusterRecord) == 64, "簇记录必须 64B");
    static_assert(sizeof(NaniteConeAxisAngle) == 16, "cone 轴角字段必须 16B");

    CHECK(sizeof(NaniteClusterRecord) == 64u);
    CHECK(alignof(NaniteClusterRecord) == 16u);
    CHECK(kNaniteClusterRecordBytes == 64u);
    CHECK(offsetof(NaniteClusterRecord, boundsCenterRadius) == 0u);
    CHECK(offsetof(NaniteClusterRecord, cone)               == 16u);
    CHECK(offsetof(NaniteClusterRecord, triangleOffset)     == 32u);
    CHECK(offsetof(NaniteClusterRecord, triangleCount)      == 36u);
    CHECK(offsetof(NaniteClusterRecord, vertexOffset)       == 40u);
    CHECK(offsetof(NaniteClusterRecord, materialID)         == 44u);
    CHECK(offsetof(NaniteClusterRecord, maxParentLODError)  == 48u);
    CHECK(offsetof(NaniteClusterRecord, childClusterOffset) == 52u);
    CHECK(offsetof(NaniteClusterRecord, childCount)         == 56u);
    CHECK(offsetof(NaniteClusterRecord, _pad)               == 60u);

    // cone 字段：xyz = 单位锥轴，w = cos(锥半角)（裁决 #1 取代 coneData）
    CHECK(sizeof(NaniteConeAxisAngle) == 16u);
    CHECK(offsetof(NaniteConeAxisAngle, axis)         == 0u);
    CHECK(offsetof(NaniteConeAxisAngle, cosHalfAngle) == 12u);
    CHECK(kNaniteConeNoCullCos == -1.0f);      // 无锥哨兵 = -1（半角 180°）

    const NaniteConeAxisAngle noCone{};
    CHECK(noCone.axis[0] == 0.0f);
    CHECK(noCone.axis[1] == 0.0f);
    CHECK(noCone.axis[2] == 0.0f);
    CHECK(noCone.cosHalfAngle == kNaniteConeNoCullCos);
    CHECK(IsValidConeAxisAngle(noCone));

    // 簇记录默认值
    const NaniteClusterRecord record{};
    CHECK(record.triangleOffset == 0u);
    CHECK(record.triangleCount  == 0u);
    CHECK(record.vertexOffset   == 0u);
    CHECK(record.materialID     == 0u);
    CHECK(record.maxParentLODError == 0.0f);
    CHECK(record.childClusterOffset == 0u);    // 0 = 叶子（§8.1 的约定）
    CHECK(record.childCount == 0u);
    CHECK(record._pad == 0u);
}

// ============================================================
// 11. 任务 7：顶点记录（16B）与量化偏置的落点（quantBias）
// ============================================================
TEST_CASE("NaniteTypes: 顶点记录尺寸与量化偏置落点（任务 7 定稿 16B）") {
    static_assert(sizeof(NaniteVertex) == 16, "顶点记录必须 16B（裁决 #7：不是 12B）");

    CHECK(sizeof(NaniteVertex) == 16u);
    CHECK(alignof(NaniteVertex) == 16u);
    CHECK(kNaniteVertexRecordBytes == 16u);
    CHECK(offsetof(NaniteVertex, packedPosition) == 0u);
    CHECK(offsetof(NaniteVertex, packedNormal)   == 4u);
    CHECK(offsetof(NaniteVertex, packedUV)       == 8u);
    CHECK(offsetof(NaniteVertex, quantBias)      == 12u);   // 裁决 #7/#9 的量化偏置落点

    const NaniteVertex vertex{};
    CHECK(vertex.quantBias == kNaniteVertexQuantBias);
    CHECK(kNaniteVertexQuantBias == 512);
    CHECK(kNaniteVertexQuantBits == 10u);
    CHECK(kNaniteVertexQuantMask == 0x3FFu);
    CHECK(kNaniteVertexQuantMin == -512);
    CHECK(kNaniteVertexQuantMax == 511);
    CHECK(kNaniteVertexQuantMax - kNaniteVertexQuantMin + 1 == 1024);   // 10 位的有符号全域

    // R10G10B10A2 位域打包/解包（§8.4 的位序：x[9:0] y[19:10] z[29:20] w[31:30]）
    const u32 packed = NanitePackR10G10B10A2(0x3FFu, 0x000u, 0x155u, 0x2u);
    CHECK(NaniteUnpackR10G10B10A2(packed, 0u) == 0x3FFu);
    CHECK(NaniteUnpackR10G10B10A2(packed, 1u) == 0x000u);
    CHECK(NaniteUnpackR10G10B10A2(packed, 2u) == 0x155u);
    CHECK(NaniteUnpackR10G10B10A2(packed, 3u) == 0x2u);

    // 位置：w 位恒为 1（§8.4 的 "R10G10B10A2_SNORM (xyz) + w=1"）
    CHECK(NaniteUnpackR10G10B10A2(NanitePackPosition(0u, 0u, 0u), 3u) == 1u);
    CHECK(NaniteUnpackR10G10B10A2(NanitePackPosition(1023u, 1023u, 1023u), 0u) == 1023u);

    // UV：R16G16_UNORM
    const u32 uv = NanitePackUV(0x1234u, 0xABCDu);
    CHECK(NaniteUnpackUVU(uv) == 0x1234u);
    CHECK(NaniteUnpackUVV(uv) == 0xABCDu);
    CHECK(NaniteUnpackUVU(NanitePackUV(0xFFFFu, 0u)) == 0xFFFFu);
    CHECK(NaniteUnpackUVV(NanitePackUV(0u, 0xFFFFu)) == 0xFFFFu);
}

// ============================================================
// 12. 任务 7：量化偏置往返（裁决 #9：编码端补 +512，与 §8.4 的解码互逆）
// ============================================================
TEST_CASE("NaniteTypes: 量化偏置往返（裁决 #9：编码端补 +512）") {
    const float bboxMin   = -1.0f;
    const float maxExtent = 4.0f;               // 量化盒 = [-1, 3]
    const float bboxMax   = bboxMin + maxExtent;

    // 两端：v = bboxMin ⇒ 有符号量 0 ⇒ raw = bias；v = bboxMax ⇒ 511 ⇒ raw = bias + 511
    const u32 rawMin = NaniteQuantizePositionAxis(bboxMin, bboxMin, maxExtent);
    const u32 rawMax = NaniteQuantizePositionAxis(bboxMax, bboxMin, maxExtent);
    CHECK(rawMin == (u32)kNaniteVertexQuantBias);
    CHECK(rawMax == (u32)(kNaniteVertexQuantBias + kNaniteVertexQuantMax));
    CHECK(rawMax == 1023u);
    CHECK(NaniteDequantizePositionAxis(rawMin, bboxMin, maxExtent) ==
          doctest::Approx(bboxMin).epsilon(1e-6));
    CHECK(NaniteDequantizePositionAxis(rawMax, bboxMin, maxExtent) ==
          doctest::Approx(bboxMax).epsilon(1e-6));

    // 中点：0.5 × 511 = 255.5 ⇒ lround = 256 ⇒ raw = 768
    const float mid = (bboxMin + bboxMax) * 0.5f;
    const u32 rawMid = NaniteQuantizePositionAxis(mid, bboxMin, maxExtent);
    CHECK(rawMid == (u32)(kNaniteVertexQuantBias + 256));
    CHECK(NaniteDequantizePositionAxis(rawMid, bboxMin, maxExtent) ==
          doctest::Approx(mid).epsilon(0.01));

    // 盒内逐点：① raw 始终落在 [bias, bias+511]（偏置确实加上了 —— 这正是旧无符号编码
    //              0…511 会解码到盒下方的原因）；② 往返误差 ≤ maxExtent/1022（半步）
    const float halfStep = maxExtent / 1022.0f;
    for (u32 i = 0; i <= 64u; ++i) {
        const float v = bboxMin + maxExtent * (float)i / 64.0f;
        const u32 raw = NaniteQuantizePositionAxis(v, bboxMin, maxExtent);
        CHECK(raw >= (u32)kNaniteVertexQuantBias);
        CHECK(raw <= (u32)(kNaniteVertexQuantBias + kNaniteVertexQuantMax));
        const float back = NaniteDequantizePositionAxis(raw, bboxMin, maxExtent);
        CHECK(std::fabs(back - v) <= halfStep + 1.0e-5f);
    }

    // 偏置的对称性：raw < bias 时解码为**负**的（= 盒下方一个量化步；旧编码 0 就落在这里）
    const float expectedNegative = bboxMin + (-512.0f / 511.0f) * maxExtent;
    CHECK(NaniteDequantizePositionAxis(0u, bboxMin, maxExtent) ==
          doctest::Approx(expectedNegative).epsilon(1e-5));
    CHECK(expectedNegative < bboxMin);
    CHECK(NaniteDequantizePositionAxis((u32)kNaniteVertexQuantBias, bboxMin, maxExtent) ==
          doctest::Approx(bboxMin).epsilon(1e-6));

    // 自定义偏置（= 记录里的 quantBias）：编解码用的是同一个值 ⇒ 自包含
    const u32 rawNoBias = NaniteQuantizePositionAxis(bboxMin, bboxMin, maxExtent, 0);
    CHECK(rawNoBias == 0u);
    CHECK(NaniteDequantizePositionAxis(rawNoBias, bboxMin, maxExtent, 0) ==
          doctest::Approx(bboxMin).epsilon(1e-6));

    // 退化轴（maxExtent = 0）：取 bias、解码回 bboxMin，且不产生除零
    CHECK(NaniteQuantizePositionAxis(123.0f, bboxMin, 0.0f) == (u32)kNaniteVertexQuantBias);
    CHECK(NaniteDequantizePositionAxis((u32)kNaniteVertexQuantBias, bboxMin, 0.0f) == bboxMin);

    // 越界值被夹到 10 位范围内（编码端绝不写出 > 1023 或负的 raw）
    CHECK(NaniteQuantizePositionAxis(1.0e9f, bboxMin, maxExtent) == 1023u);
    CHECK(NaniteQuantizePositionAxis(-1.0e9f, bboxMin, maxExtent) == 0u);
    CHECK(NaniteClampRaw10(-1) == 0u);
    CHECK(NaniteClampRaw10(1024) == 1023u);
    CHECK(NaniteClampRaw10(768) == 768u);
}

// ============================================================
// 13. 任务 7：三角形索引编码（裁决 #6：3×u16 进 u32[2]）的边界
// ============================================================
TEST_CASE("NaniteTypes: 三角形索引编码边界（裁决 #6：3×u16 进 u32[2]）") {
    static_assert(sizeof(NanitePackedTriangle) == 8, "打包三角形必须 8B");

    CHECK(sizeof(NanitePackedTriangle) == 8u);
    CHECK(offsetof(NanitePackedTriangle, lo) == 0u);
    CHECK(offsetof(NanitePackedTriangle, hi) == 4u);
    CHECK(kNaniteIndexBytesPerTriangle == 8u);
    CHECK(kNaniteIndicesPerTriangle == 3u);
    CHECK(kNaniteMaxClusterTriangles == 64u);
    CHECK(kNaniteMaxClusterVertices == 128u);
    CHECK(kNaniteIndexMaxU16 == 0xFFFFu);

    // 位布局：lo = i0 | (i1 << 16)、hi = i2（高 16 位保留 0）
    const NanitePackedTriangle triangle = NanitePackTriangle(1u, 2u, 3u);
    CHECK(triangle.lo == 0x00020001u);
    CHECK(triangle.hi == 0x00000003u);
    CHECK(NaniteTriangleIndex0(triangle) == 1u);
    CHECK(NaniteTriangleIndex1(triangle) == 2u);
    CHECK(NaniteTriangleIndex2(triangle) == 3u);

    // 全 0 与 u16 位边界：高位不互相污染
    const NanitePackedTriangle zero = NanitePackTriangle(0u, 0u, 0u);
    CHECK(zero.lo == 0u);
    CHECK(zero.hi == 0u);
    const NanitePackedTriangle allOnes = NanitePackTriangle(0xFFFFu, 0xFFFFu, 0xFFFFu);
    CHECK(allOnes.lo == 0xFFFFFFFFu);
    CHECK(allOnes.hi == 0x0000FFFFu);      // 高 16 位是保留位，不写 1
    CHECK(NaniteTriangleIndex0(allOnes) == 0xFFFFu);
    CHECK(NaniteTriangleIndex1(allOnes) == 0xFFFFu);
    CHECK(NaniteTriangleIndex2(allOnes) == 0xFFFFu);

    // 超出 u16 的高位被丢弃（不跨字段污染）
    const NanitePackedTriangle masked = NanitePackTriangle(0x1FFFFu, 0u, 0u);
    CHECK(masked.lo == 0x0000FFFFu);
    CHECK(NaniteTriangleIndex0(masked) == 0xFFFFu);

    // 语义边界：簇内局部下标（"每簇 ≤128 顶点" ⇒ 合法区间 [0,127]）
    CHECK(IsValidClusterLocalVertexIndex(0u));
    CHECK(IsValidClusterLocalVertexIndex(kNaniteMaxClusterVertices - 1u));
    CHECK_FALSE(IsValidClusterLocalVertexIndex(kNaniteMaxClusterVertices));   // 128 越界
    CHECK_FALSE(IsValidClusterLocalVertexIndex(0xFFFFu));

    // 一簇的最大索引块 = 64 三角形 × 8B = 512B，天然 16B 对齐（裁决 #6 的取舍依据）
    CHECK((kNaniteMaxClusterTriangles * kNaniteIndexBytesPerTriangle) % kNaniteFileAlignment == 0u);
}

// ============================================================
// 14. 任务 7：cone 数据解码（裁决 #1：coneAxisAngle = 单位轴 + cos 半角）
// ============================================================
TEST_CASE("NaniteTypes: cone 数据解码（裁决 #1：coneAxisAngle）") {
    const float kPi = std::acos(-1.0f);

    // 30° 半角、轴 = -Z
    NaniteConeAxisAngle cone{};
    cone.axis[0] = 0.0f;
    cone.axis[1] = 0.0f;
    cone.axis[2] = -1.0f;
    cone.cosHalfAngle = std::cos(kPi / 6.0f);
    CHECK(IsValidConeAxisAngle(cone));
    CHECK(NaniteConeHalfAngleRadians(cone.cosHalfAngle) == doctest::Approx(kPi / 6.0f).epsilon(1e-4));

    // 轴可以是任意单位方向
    NaniteConeAxisAngle diagonal{};
    const float invSqrt3 = 1.0f / std::sqrt(3.0f);
    diagonal.axis[0] = invSqrt3;
    diagonal.axis[1] = invSqrt3;
    diagonal.axis[2] = invSqrt3;
    diagonal.cosHalfAngle = 0.5f;                  // 60°
    CHECK(IsValidConeAxisAngle(diagonal));
    CHECK(NaniteConeHalfAngleRadians(diagonal.cosHalfAngle) == doctest::Approx(kPi / 3.0f).epsilon(1e-4));

    // 无锥哨兵（w = -1）：轴允许为 0 向量，半角 = 180°（恒不可剔除）
    NaniteConeAxisAngle noCone{};
    noCone.cosHalfAngle = kNaniteConeNoCullCos;
    CHECK(IsValidConeAxisAngle(noCone));
    CHECK(IsValidConeAxisAngle(0.0f, 0.0f, 0.0f, kNaniteConeNoCullCos));
    CHECK(NaniteConeHalfAngleRadians(noCone.cosHalfAngle) == doctest::Approx(kPi).epsilon(1e-6));

    // 半开锥（cos = 0 ⇒ 90°）与针尖锥（cos = 1 ⇒ 0°）
    CHECK(NaniteConeHalfAngleRadians(0.0f) == doctest::Approx(kPi * 0.5f).epsilon(1e-6));
    CHECK(NaniteConeHalfAngleRadians(1.0f) == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(IsValidConeAxisAngle(0.0f, 0.0f, 1.0f, 1.0f));
    CHECK(NaniteConeHalfAngleRadians(2.0f) == doctest::Approx(0.0f).epsilon(1e-6));   // 越界被夹

    // 反例：非单位轴 / 零轴（且不是哨兵）/ cos 越界 / NaN
    CHECK_FALSE(IsValidConeAxisAngle(0.0f, 0.0f, -1.5f, 0.5f));
    CHECK_FALSE(IsValidConeAxisAngle(0.0f, 0.0f, 0.0f, 0.5f));
    CHECK_FALSE(IsValidConeAxisAngle(0.0f, 0.0f, -1.0f, 1.5f));
    CHECK_FALSE(IsValidConeAxisAngle(0.0f, 0.0f, -1.0f, -1.5f));
    const float nan = std::nanf("");
    CHECK_FALSE(IsValidConeAxisAngle(0.0f, 0.0f, -1.0f, nan));
    CHECK(kNaniteConeAxisTolerance == 1.0e-3f);
}

// ============================================================
// 15. 任务 7：段表推导（计数 → 各段 offset/size、16B 对齐、总长）
// ============================================================
TEST_CASE("NaniteTypes: .nanite 段表推导（计数 → offset/size）") {
    NaniteFileHeader header{};
    header.clusterCount  = 3u;
    header.vertexCount   = 10u;
    header.indexCount    = 30u;   // 10 个三角形 × 3
    header.materialCount = 2u;
    header.lodLevelCount = 1u;

    NaniteFileLayout layout{};
    REQUIRE(TryBuildNaniteFileLayout(header, layout) == NaniteFileError::None);
    CHECK(layout.headerOffset   == 0u);
    CHECK(layout.headerBytes    == 96u);
    CHECK(layout.clusterOffset  == 96u);
    CHECK(layout.clusterBytes   == 3u * 64u);
    CHECK(layout.vertexOffset   == 96u + 192u);
    CHECK(layout.vertexBytes    == 10u * 16u);
    CHECK(layout.indexOffset    == 288u + 160u);
    CHECK(layout.indexBytes     == 10u * 8u);     // 80B 已经是 16 的倍数
    CHECK(layout.materialOffset == 448u + 80u);
    CHECK(layout.materialBytes  == 2u * 8u);
    CHECK(layout.lodOffset      == 528u + 16u);
    CHECK(layout.lodBytes       == 16u);          // 1 × 4B 向上取整到 16B
    CHECK(layout.totalBytes     == 544u + 16u);
    CHECK(layout.triangleCount  == 10u);
    CHECK(layout.totalBytes % kNaniteFileAlignment == 0u);

    // 属性循环：任意计数下每个段的起点都 16B 对齐（⇒ `Misaligned` 分支不可达），且段首尾相接
    const u32 counts[] = { 0u, 1u, 3u, 7u, 64u, 1001u };
    for (u32 clusterCount : counts) {
        for (u32 vertexCount : counts) {
            NaniteFileHeader probe{};
            probe.clusterCount  = clusterCount;
            probe.vertexCount   = vertexCount;
            probe.materialCount = clusterCount;    // 让材质段出现奇数个（8B 步长的对齐压力）
            probe.lodLevelCount = vertexCount;
            probe.indexCount    = clusterCount * kNaniteIndicesPerTriangle;

            NaniteFileLayout probeLayout{};
            REQUIRE(TryBuildNaniteFileLayout(probe, probeLayout) == NaniteFileError::None);
            CHECK(probeLayout.clusterOffset  % kNaniteFileAlignment == 0u);
            CHECK(probeLayout.vertexOffset   % kNaniteFileAlignment == 0u);
            CHECK(probeLayout.indexOffset    % kNaniteFileAlignment == 0u);
            CHECK(probeLayout.materialOffset % kNaniteFileAlignment == 0u);
            CHECK(probeLayout.lodOffset      % kNaniteFileAlignment == 0u);
            CHECK(probeLayout.totalBytes     % kNaniteFileAlignment == 0u);
            CHECK(probeLayout.clusterOffset  == probeLayout.headerOffset + probeLayout.headerBytes);
            CHECK(probeLayout.vertexOffset   == probeLayout.clusterOffset + probeLayout.clusterBytes);
            CHECK(probeLayout.indexOffset    == probeLayout.vertexOffset + probeLayout.vertexBytes);
            CHECK(probeLayout.materialOffset == probeLayout.indexOffset + probeLayout.indexBytes);
            CHECK(probeLayout.lodOffset      == probeLayout.materialOffset + probeLayout.materialBytes);
            CHECK(probeLayout.totalBytes     == probeLayout.lodOffset + probeLayout.lodBytes);
        }
    }

    // indexCount 不是 3 的倍数 ⇒ BadIndexCount（无法按 3×u16 打包），且不改写出参
    NaniteFileHeader badTriangles{};
    badTriangles.indexCount = 4u;
    NaniteFileLayout untouched{};
    untouched.totalBytes = 0xEEEEu;
    CHECK(TryBuildNaniteFileLayout(badTriangles, untouched) == NaniteFileError::BadIndexCount);
    CHECK(untouched.totalBytes == 0xEEEEu);
}

// ============================================================
// 16. 任务 7：校验函数的正例与反例（空指针/截断/魔数/版本/索引数/越界/尾部多余）
// ============================================================
TEST_CASE("NaniteTypes: .nanite 校验函数的正例与反例") {
    NaniteFileHeader header{};
    header.clusterCount  = 3u;
    header.vertexCount   = 10u;
    header.indexCount    = 30u;
    header.materialCount = 2u;
    header.lodLevelCount = 1u;

    std::vector<u8> buffer;
    const NaniteFileLayout layout = BuildNaniteBuffer(header, buffer);
    REQUIRE(layout.totalBytes == 560u);
    REQUIRE(buffer.size() == layout.totalBytes);

    // ① 正例：完整文件 ⇒ None，且段表与推导一致
    NaniteFileLayout parsed{};
    CHECK(ValidateNaniteFile(buffer.data(), buffer.size(), &parsed) == NaniteFileError::None);
    CHECK(parsed.totalBytes   == layout.totalBytes);
    CHECK(parsed.indexOffset  == layout.indexOffset);
    CHECK(parsed.materialOffset == layout.materialOffset);
    CHECK(parsed.triangleCount == 10u);
    CHECK(ValidateNaniteHeader(buffer.data(), buffer.size()));

    // ② 尾部多余字节仍然合法（将来可追加调试信息）
    buffer.resize(layout.totalBytes + 64u, 0u);
    CHECK(ValidateNaniteFile(buffer.data(), buffer.size()) == NaniteFileError::None);
    buffer.resize(layout.totalBytes);

    // ③ 空指针与截断：连 96B 头部都读不出来 ⇒ TooSmall；差 1 字节 ⇒ OutOfBounds
    CHECK(ValidateNaniteFile(nullptr, layout.totalBytes) == NaniteFileError::NullData);
    CHECK(ValidateNaniteFile(buffer.data(), 0u)  == NaniteFileError::TooSmall);
    CHECK(ValidateNaniteFile(buffer.data(), 1u)  == NaniteFileError::TooSmall);
    CHECK(ValidateNaniteFile(buffer.data(), 95u) == NaniteFileError::TooSmall);
    CHECK(ValidateNaniteFile(buffer.data(), layout.totalBytes - 1u) == NaniteFileError::OutOfBounds);
    CHECK_FALSE(ValidateNaniteHeader(buffer.data(), layout.totalBytes - 1u));

    // ④ 魔数：8 个字节里任意一个错都能被发现
    for (u32 i = 0; i < 8u; ++i) {
        buffer[i] = (u8)'X';
        CHECK(ValidateNaniteFile(buffer.data(), buffer.size()) == NaniteFileError::BadMagic);
        buffer[i] = (u8)kNaniteFileMagic[i];
    }
    CHECK(ValidateNaniteFile(buffer.data(), buffer.size()) == NaniteFileError::None);

    // ⑤ 版本错
    {
        std::vector<u8> bad = buffer;
        NaniteFileHeader wrongVersion = header;
        wrongVersion.version = 2u;
        std::memcpy(bad.data(), &wrongVersion, sizeof(wrongVersion));
        CHECK(ValidateNaniteFile(bad.data(), bad.size()) == NaniteFileError::BadVersion);
    }

    // ⑥ indexCount 不是 3 的倍数（索引编码的前提）
    {
        std::vector<u8> bad = buffer;
        NaniteFileHeader wrongIndices = header;
        wrongIndices.indexCount = 31u;
        std::memcpy(bad.data(), &wrongIndices, sizeof(wrongIndices));
        CHECK(ValidateNaniteFile(bad.data(), bad.size()) == NaniteFileError::BadIndexCount);
    }

    // ⑦ 头部计数巨大 ⇒ 段表总长远超缓冲 ⇒ OutOfBounds（不越界读、u64 内不溢出）
    {
        std::vector<u8> bad = buffer;
        NaniteFileHeader hugeClusters = header;
        hugeClusters.clusterCount = 0xFFFFFFFFu;
        std::memcpy(bad.data(), &hugeClusters, sizeof(hugeClusters));
        CHECK(ValidateNaniteFile(bad.data(), bad.size()) == NaniteFileError::OutOfBounds);

        NaniteFileLayout sentinel{};
        sentinel.totalBytes = 0xEEEEu;
        CHECK(ValidateNaniteFile(bad.data(), bad.size(), &sentinel) == NaniteFileError::OutOfBounds);
        CHECK(sentinel.totalBytes == 0xEEEEu);   // 失败时不改写出参
    }
    {
        std::vector<u8> bad = buffer;
        NaniteFileHeader hugeVertices = header;
        hugeVertices.vertexCount = 0xFFFFFFFFu;
        std::memcpy(bad.data(), &hugeVertices, sizeof(hugeVertices));
        CHECK(ValidateNaniteFile(bad.data(), bad.size()) == NaniteFileError::OutOfBounds);
    }

    // ⑧ 全零计数：只有 96B 头部的最小文件也是合法的
    {
        const NaniteFileHeader empty{};
        std::vector<u8> justHeader(sizeof(NaniteFileHeader), 0u);
        std::memcpy(justHeader.data(), &empty, sizeof(empty));
        NaniteFileLayout emptyLayout{};
        CHECK(ValidateNaniteFile(justHeader.data(), justHeader.size(), &emptyLayout) ==
              NaniteFileError::None);
        CHECK(emptyLayout.totalBytes == 96u);
        CHECK(emptyLayout.triangleCount == 0u);
    }

    // ⑨ 失败原因的名字可读（断言与日志共用）
    CHECK(NaniteFileErrorName(NaniteFileError::None) != nullptr);
    CHECK(std::strcmp(NaniteFileErrorName(NaniteFileError::None), "None") == 0);
    CHECK(std::strcmp(NaniteFileErrorName(NaniteFileError::NullData), "NullData") == 0);
    CHECK(std::strcmp(NaniteFileErrorName(NaniteFileError::TooSmall), "TooSmall") == 0);
    CHECK(std::strcmp(NaniteFileErrorName(NaniteFileError::BadMagic), "BadMagic") == 0);
    CHECK(std::strcmp(NaniteFileErrorName(NaniteFileError::BadVersion), "BadVersion") == 0);
    CHECK(std::strcmp(NaniteFileErrorName(NaniteFileError::BadIndexCount), "BadIndexCount") == 0);
    CHECK(std::strcmp(NaniteFileErrorName(NaniteFileError::Misaligned), "Misaligned") == 0);
    CHECK(std::strcmp(NaniteFileErrorName(NaniteFileError::OutOfBounds), "OutOfBounds") == 0);
}
