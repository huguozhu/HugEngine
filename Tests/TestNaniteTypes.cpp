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
//  15b. 任务 11：段对齐原语 `NaniteAlignUpFile` 与 LOD 段步长的边界（偏移契约的原语层）
//  16. 任务 7：校验函数的正例与反例（空指针/截断/魔数错/版本错/索引数错/越界/尾部多余）
//
// §14.8 任务 10（量化编解码的验收，全部在本文件；打包/段布局在 TestNaniteBuilder.cpp）：
//  12. 位置量化往返：**任务 10 的基准裁决**（盒心 origin + 乘数 1022 ⇒ 吃满 10 位），
//      中点/端点/四分点/误差上界 range/2044/越界 clamp/退化轴/NaN
//  12b. 位置量化**无 clamp** 的口径证明（簇心 + 网格最大范围；极端簇 + 多组中心/半轴扫描）
//  12c. 法线八面体编码 10+10 位：轴/对角/符号边界、Fibonacci 球 20000 方向的**最坏角误差**
//  12d. UV unorm16：4097 点往返误差、越界 clamp 的如实口径、10 位 UNORM 辅助
//  12e. 材质 8B 打包/解包（含字节序与越界 word）
// ============================================================

#include "doctest.h"

#include "Nanite/NaniteTypes.h"   // 分区契约 + 实例槽分配器 + 任务 3/4 的 POD（RHI-free）

// 【任务 13】视锥提取的对照物：引擎既有的 he::Frustum / he::Sphere（RHI-free，只依赖 Core/Math）。
// 用它做交叉验证，保证 NaniteTypes.h 的提取与判据同引擎口径，而不是自成一套。
#include "Math/Geometry.h"

#include <algorithm> // std::max（任务 10：法线八面体的稠密采样）
#include <cmath>     // std::fabs / std::acos（cone 解码与量化误差）
#include <cstddef>   // offsetof
#include <cstring>   // memcpy（把头部写进测试缓冲）
#include <ostream>   // 【任务 13】doctest 的 MESSAGE 需要完整的 std::ostream
#include <string>    // 【任务 13】CPU 参考剔除用例的 MESSAGE 拼串
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
// 12. 量化往返（裁决 #9 的偏置 + **任务 10 的基准裁决**：盒心 + 吃满 10 位）
//
// 【任务 10 改了什么】任务 7 的原口径（§8.4）以 `bboxMin` 为原点、把 `[bboxMin, bboxMin+range]`
//   映射到有符号 [0,511] ⇒ 盒内只用到上半段（raw ∈ [512,1023]），等效 ~9 位精度。
//   任务 10 改为"以盒**中心**为原点、把 `[origin-range/2, origin+range/2]` 映射到 [-512,511]"
//   （乘数 **1022**）⇒ **吃满 10 位**，往返误差上界从 range/1022 收到 range/2044。
//   本用例把新口径的中点/端点/全量程/误差上界/无 clamp 逐点钉住 —— 旧断言（raw ∈ [bias,
//   bias+511]、误差 ≤ range/1022）按新口径更新，这正是 §8.4 "留给任务 10 按量化误差验收决定"
//   的那条已知取舍的落点。
// ============================================================
TEST_CASE("NaniteTypes: 位置量化往返（任务 10 基准：盒心 + 吃满 10 位）") {
    const float origin = -1.0f;   // 盒**中心**（= 簇 AABB 中心的口径）
    const float range  = 4.0f;    // 量化范围 ⇒ 可表示区间 = [origin-range/2, origin+range/2] = [-3, 3]
    const float low    = origin - range * 0.5f;
    const float high   = origin + range * 0.5f;

    // 中点（signed = 0 ⇒ raw = bias）与两端（signed = ∓511 ⇒ raw = 1 / 1023）
    const u32 rawCenter = NaniteQuantizePositionAxis(origin, origin, range);
    const u32 rawLow    = NaniteQuantizePositionAxis(low, origin, range);
    const u32 rawHigh   = NaniteQuantizePositionAxis(high, origin, range);
    CHECK(rawCenter == (u32)kNaniteVertexQuantBias);      // 512
    CHECK(rawLow == 1u);                                  // -511 + 512
    CHECK(rawHigh == 1023u);                              // +511 + 512
    CHECK(rawLow < (u32)kNaniteVertexQuantBias);          // 负半段**真的被用到**（旧口径用不到）
    CHECK(rawHigh > (u32)kNaniteVertexQuantBias);
    CHECK(kNaniteVertexQuantFullScale == 1022);
    CHECK(kNaniteVertexQuantMin == -512);
    CHECK(kNaniteVertexQuantMax == 511);

    // 端点解码（对称性）
    CHECK(NaniteDequantizePositionAxis(rawCenter, origin, range) ==
          doctest::Approx(origin).epsilon(1e-6));
    CHECK(NaniteDequantizePositionAxis(rawLow, origin, range) ==
          doctest::Approx(low).epsilon(1e-6));
    CHECK(NaniteDequantizePositionAxis(rawHigh, origin, range) ==
          doctest::Approx(high).epsilon(1e-6));

    // 盒内逐点：① raw 覆盖 [1, 1023]（两端都用上 ⇒ 10 位吃满）；
    //            ② 往返误差 ≤ range/2044（**半个量化步**，比任务 7 的 range/1022 再小一半）；
    //            ③ 盒内一律不 clamp
    const float halfStep = range / (2.0f * (float)kNaniteVertexQuantFullScale);
    float maxError = 0.0f;
    for (u32 i = 0; i <= 256u; ++i) {
        const float v   = low + range * (float)i / 256.0f;
        const u32   raw = NaniteQuantizePositionAxis(v, origin, range);
        CHECK(raw >= 1u);
        CHECK(raw <= 1023u);
        CHECK_FALSE(NanitePositionQuantizeClamps(v, origin, range));
        const float error = std::fabs(NaniteDequantizePositionAxis(raw, origin, range) - v);
        if (error > maxError) maxError = error;
        CHECK(error <= halfStep + 1.0e-6f);
    }
    MESSAGE("位置量化：range=4 实测最大往返误差=" << maxError
            << "  上界 range/2044=" << halfStep);

    // 四分之一点（v = origin + range/4 = 0）：signed = 255.5 ⇒ lround = 256 ⇒ raw = 768
    CHECK(NaniteQuantizePositionAxis(origin + range * 0.25f, origin, range) ==
          (u32)(kNaniteVertexQuantBias + 256));

    // 越出可表示区间：signed 被夹到边界（编码端绝不写出 >1023 / <0 的 raw），且诊断函数能识别
    CHECK(NaniteQuantizePositionAxis(origin + range, origin, range) == 1023u);
    CHECK(NaniteQuantizePositionAxis(origin - range, origin, range) == 0u);
    CHECK(NanitePositionQuantizeClamps(origin + range, origin, range));
    CHECK(NanitePositionQuantizeClamps(origin - range, origin, range));
    CHECK_FALSE(NanitePositionQuantizeClamps(high, origin, range));   // 恰好端点：不算 clamp
    CHECK_FALSE(NanitePositionQuantizeClamps(low, origin, range));
    CHECK(NaniteClampRaw10(-1) == 0u);
    CHECK(NaniteClampRaw10(1024) == 1023u);
    CHECK(NaniteClampRaw10(768) == 768u);

    // raw = 0（signed = -512）解码到区间**下方**一个量化步：负半段是真实有意义的码点
    const float belowLow = NaniteDequantizePositionAxis(0u, origin, range);
    CHECK(belowLow < low);
    CHECK(belowLow == doctest::Approx(origin + (-512.0f / 1022.0f) * range).epsilon(1e-5));

    // 自定义偏置（= 记录里的 quantBias 字段）：编解码用同一个值 ⇒ 自包含
    CHECK(NaniteQuantizePositionAxis(origin, origin, range, 0) == 0u);
    CHECK(NaniteDequantizePositionAxis(0u, origin, range, 0) ==
          doctest::Approx(origin).epsilon(1e-6));

    // 退化轴（range = 0）：取 bias、解码回 origin；不产生除零，也不算 clamp
    CHECK(NaniteQuantizePositionAxis(123.0f, origin, 0.0f) == (u32)kNaniteVertexQuantBias);
    CHECK(NaniteDequantizePositionAxis((u32)kNaniteVertexQuantBias, origin, 0.0f) == origin);
    CHECK_FALSE(NanitePositionQuantizeClamps(123.0f, origin, 0.0f));

    // NaN 输入：编码端给 bias（不产生未定义行为），且不算 clamp
    const float nan = std::nanf("");
    CHECK(NaniteQuantizePositionAxis(nan, origin, range) == (u32)kNaniteVertexQuantBias);
    CHECK_FALSE(NanitePositionQuantizeClamps(nan, origin, range));
}

// ============================================================
// 12b. 任务 10：位置量化的**无 clamp** 口径证明（簇 AABB 中心 + 网格最大范围）
//
// 证明（也是打包器里 positionClampCount 恒为 0 的依据）：簇是网格的子集 ⇒
//   每轴 |v - 簇心_轴| ≤ 簇局部半轴长 ≤ 该轴网格范围/2 ≤ meshExtent/2 = range/2
//   ⇒ |signed| ≤ 511 ≤ [−512, 511] ⇒ 恒不 clamp。
// 下面用"极端簇"（AABB 恰好铺满网格最大轴，半轴长 = range/2）+ 多组中心/尺度扫描把它变成读数。
// ============================================================
TEST_CASE("NaniteTypes: 位置量化无 clamp（簇心 + 网格最大范围口径）") {
    const float range = 8.0f;   // meshExtent

    // ① 极端簇：AABB 沿 x 铺满整个网格最大范围 ⇒ 半轴长恰好 = range/2
    const float originX = 4.0f;
    u32 minRaw = 1023u;
    u32 maxRaw = 0u;
    for (u32 i = 0; i <= 64u; ++i) {
        const float x   = range * (float)i / 64.0f;   // [0, 8]
        const u32   raw = NaniteQuantizePositionAxis(x, originX, range);
        CHECK_FALSE(NanitePositionQuantizeClamps(x, originX, range));
        if (raw < minRaw) minRaw = raw;
        if (raw > maxRaw) maxRaw = raw;
    }
    CHECK(minRaw == 1u);      // x = 0 ⇒ signed = -511（负半段用满）
    CHECK(maxRaw == 1023u);   // x = 8 ⇒ signed = +511（正半段用满）
    MESSAGE("位置量化无 clamp：极端簇实测 raw 区间=[" << minRaw << "," << maxRaw
            << "] range=" << range);

    // ② 多组 (中心, 半轴长) 扫描：只要半轴长 ≤ range/2 就不 clamp；超过就必然 clamp（诊断可见）
    const float centers[5] = { 0.0f, -3.5f, 1.25f, 100.0f, -1000.0f };
    const float halves[4]  = { 0.0f, range * 0.25f, range * 0.49f, range * 0.5f };
    for (const float center : centers) {
        for (const float half : halves) {
            for (u32 i = 0; i <= 32u; ++i) {
                const float offset = -half + 2.0f * half * (float)i / 32.0f;   // [-half, +half]
                const float v      = center + offset;
                CHECK_FALSE(NanitePositionQuantizeClamps(v, center, range));
                const u32 raw = NaniteQuantizePositionAxis(v, center, range);
                CHECK(raw >= 1u);
                CHECK(raw <= 1023u);
            }
        }
    }

    // ③ 反例（防御性）：半轴长超过 range/2 时诊断函数必须能识别出 clamp
    CHECK(NanitePositionQuantizeClamps(8.0f, 0.0f, range));      // |v-origin| = range > range/2
    CHECK(NanitePositionQuantizeClamps(-4.1f, 0.0f, range));
    CHECK_FALSE(NanitePositionQuantizeClamps(4.0f, 0.0f, range)); // 恰好 range/2：不 clamp
}

// ============================================================
// 12c. 任务 10：法线八面体编码（10+10 位进 packedNormal 的 x/y 域）
// ============================================================
TEST_CASE("NaniteTypes: 法线八面体编码往返（10+10 位，角误差 ≤ 0.5°）") {
    const float kPi    = std::acos(-1.0f);
    const float toDeg  = 180.0f / kPi;
    CHECK(kNaniteNormalOctahedralBits == 10u);
    CHECK(kNaniteNormalAngleErrorBoundDegrees == 0.5f);

    /// 局部 lambda：编码 → 解包 → 角误差（度）
    const auto angleErrorDegrees = [](float nx, float ny, float nz, const NaniteVertex& vertex) {
        const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (!(length > 0.0f)) return 0.0f;
        float dx = 0.0f, dy = 0.0f, dz = 0.0f;
        NaniteUnpackNormal(vertex.packedNormal, dx, dy, dz);
        return NaniteNormalAngleErrorRadians(nx / length, ny / length, nz / length, dx, dy, dz) *
               (180.0f / std::acos(-1.0f));
    };

    // ① 六个轴向 + 八个对角：位域（z/w 保留 0）+ 角误差
    const float axes[][3] = {
        {  1.0f,  0.0f,  0.0f }, { -1.0f,  0.0f,  0.0f },
        {  0.0f,  1.0f,  0.0f }, {  0.0f, -1.0f,  0.0f },
        {  0.0f,  0.0f,  1.0f }, {  0.0f,  0.0f, -1.0f },
        {  1.0f,  1.0f,  1.0f }, { -1.0f,  1.0f, -1.0f },
        {  1.0f, -1.0f,  1.0f }, { -1.0f, -1.0f,  1.0f },
        {  0.0f,  0.0f, -1.0f }, {  0.0f, -1.0f,  0.0f },   // 折叠路径里 x/y 恰为 0 的符号边界
    };
    for (const auto& axis : axes) {
        NaniteVertex vertex;
        vertex.packedNormal = NanitePackNormal(axis[0], axis[1], axis[2]);
        // 八面体只占 x/y 两个 10 位域：z 与 w 必须保留 0（§8.4 的位域契约）
        CHECK(NaniteUnpackR10G10B10A2(vertex.packedNormal, 2u) == 0u);
        CHECK(NaniteUnpackR10G10B10A2(vertex.packedNormal, 3u) == 0u);
        const float error = angleErrorDegrees(axis[0], axis[1], axis[2], vertex);
        CHECK(error <= kNaniteNormalAngleErrorBoundDegrees);
    }

    // ② 稠密采样（Fibonacci 球，确定性、无随机数）：把最坏角误差变成读数
    const u32   sampleCount   = 20000u;
    const float goldenAngle   = kPi * (3.0f - std::sqrt(5.0f));
    float       maxError      = 0.0f;
    float       maxErrorAt[3] = { 0.0f, 0.0f, 1.0f };
    for (u32 i = 0; i < sampleCount; ++i) {
        const float z     = 1.0f - 2.0f * ((float)i + 0.5f) / (float)sampleCount;
        const float radius = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const float phi    = goldenAngle * (float)i;
        const float nx = radius * std::cos(phi);
        const float ny = radius * std::sin(phi);
        NaniteVertex vertex;
        vertex.packedNormal = NanitePackNormal(nx, ny, z);
        const float error = angleErrorDegrees(nx, ny, z, vertex);
        if (error > maxError) {
            maxError = error;
            maxErrorAt[0] = nx; maxErrorAt[1] = ny; maxErrorAt[2] = z;
        }
    }
    MESSAGE("法线八面体 10+10 位：" << sampleCount << " 个方向实测最大角误差=" << maxError
            << "°（出现在 " << maxErrorAt[0] << "," << maxErrorAt[1] << "," << maxErrorAt[2]
            << "）阈值=" << kNaniteNormalAngleErrorBoundDegrees << "°");
    CHECK(maxError <= kNaniteNormalAngleErrorBoundDegrees);
    CHECK(maxError > 0.0f);   // 确实发生了量化（不是恒等映射）

    // ③ 解码结果必须是单位向量（八面体解码自带归一化）
    float dx = 0.0f, dy = 0.0f, dz = 0.0f;
    NaniteUnpackNormal(NanitePackNormal(0.3f, -0.5f, 0.81f), dx, dy, dz);
    CHECK(std::sqrt(dx * dx + dy * dy + dz * dz) == doctest::Approx(1.0f).epsilon(1e-5f));

    // ④ 退化输入：零向量 / NaN ⇒ 定点 +Z（编解码自洽，不产生 NaN）
    for (const float bad : { 0.0f, std::nanf("") }) {
        NaniteVertex vertex;
        vertex.packedNormal = NanitePackNormal(bad, bad, bad);
        NaniteUnpackNormal(vertex.packedNormal, dx, dy, dz);
        CHECK(dx == doctest::Approx(0.0f).epsilon(0.01f));
        CHECK(dy == doctest::Approx(0.0f).epsilon(0.01f));
        CHECK(dz == doctest::Approx(1.0f).epsilon(1e-5f));
    }
    // 零向量的八面体坐标恒为 (0,0)（两位域都落在中点 512 附近，不是 0 —— 见 UV/UNORM 口径）
    CHECK(NaniteUnpackR10G10B10A2(NanitePackNormal(0.0f, 0.0f, 0.0f), 0u) == 512u);
    CHECK(NaniteUnpackR10G10B10A2(NanitePackNormal(0.0f, 0.0f, 0.0f), 1u) == 512u);
}

// ============================================================
// 12d. 任务 10：UV unorm16 量化（含越界 clamp 的如实口径）
// ============================================================
TEST_CASE("NaniteTypes: UV unorm16 量化往返（≤ 1/65535）与越界 clamp") {
    CHECK(kNaniteUVQuantMax == 0xFFFFu);

    // 端点与 clamp（unorm16 表示不了越界 UV，按 §8.4 的口径夹到 [0,1]）
    CHECK(NaniteQuantizeUV(0.0f) == 0u);
    CHECK(NaniteQuantizeUV(1.0f) == 0xFFFFu);
    CHECK(NaniteQuantizeUV(0.5f) == 32768u);      // round(0.5 × 65535) = round(32767.5) = 32768
    CHECK(NaniteQuantizeUV(-0.5f) == 0u);
    CHECK(NaniteQuantizeUV(2.0f) == 0xFFFFu);
    CHECK(NaniteQuantizeUV(std::nanf("")) == 0u);
    CHECK(NaniteUVNeedsClamp(-0.001f));
    CHECK(NaniteUVNeedsClamp(1.001f));
    CHECK(NaniteUVNeedsClamp(std::nanf("")));
    CHECK_FALSE(NaniteUVNeedsClamp(0.0f));
    CHECK_FALSE(NaniteUVNeedsClamp(0.5f));
    CHECK_FALSE(NaniteUVNeedsClamp(1.0f));

    // 往返误差 ≤ 1/65535（**一个量化步**；半个步是 1/131070）
    const float bound    = 1.0f / (float)kNaniteUVQuantMax;
    const float halfStep = 0.5f * bound;
    float maxError = 0.0f;
    for (u32 i = 0; i <= 4096u; ++i) {
        const float v   = (float)i / 4096.0f;
        const u32   raw = NaniteQuantizeUV(v);
        const float back = NaniteDequantizeUV(raw);
        const float error = std::fabs(back - v);
        if (error > maxError) maxError = error;
        CHECK(error <= halfStep + 1.0e-7f);
    }
    MESSAGE("UV unorm16：4097 点实测最大往返误差=" << maxError
            << "  半个量化步=1/131070=" << halfStep << "  阈值 1/65535=" << bound);
    CHECK(maxError <= bound);

    // 位序：R16G16_UNORM（u 低 16 位、v 高 16 位），u/v 不互相污染
    const u32 packed = NanitePackUV(NaniteQuantizeUV(0.25f), NaniteQuantizeUV(0.75f));
    CHECK(NaniteUnpackUVU(packed) == NaniteQuantizeUV(0.25f));
    CHECK(NaniteUnpackUVV(packed) == NaniteQuantizeUV(0.75f));
    CHECK(NaniteUnpackUVU(NanitePackUV(0xFFFFu, 0u)) == 0xFFFFu);
    CHECK(NaniteUnpackUVV(NanitePackUV(0u, 0xFFFFu)) == 0xFFFFu);

    // 10 位 UNORM 辅助（法线八面体用）：[-1,1] ↔ [0,1023] 的端点与中点
    CHECK(NaniteQuantizeUNorm10(-1.0f) == 0u);
    CHECK(NaniteQuantizeUNorm10(1.0f) == 1023u);
    CHECK(NaniteDequantizeUNorm10(0u) == doctest::Approx(-1.0f).epsilon(1e-6));
    CHECK(NaniteDequantizeUNorm10(1023u) == doctest::Approx(1.0f).epsilon(1e-6));
    CHECK(NaniteQuantizeUNorm10(-5.0f) == 0u);       // 越界 clamp
    CHECK(NaniteQuantizeUNorm10(5.0f) == 1023u);
    CHECK(NaniteQuantizeUNorm10(std::nanf("")) == 0u);
}

// ============================================================
// 12e. 任务 10：材质记录 8B 打包/解包（字段按 §8 定稿，只有两个 bindless 纹理 ID）
// ============================================================
TEST_CASE("NaniteTypes: 材质记录 8B 打包/解包（任务 10）") {
    static_assert(sizeof(NaniteMaterialRecord) == 8, "材质记录必须 8B");
    CHECK(kNaniteMaterialRecordBytes == 8u);
    CHECK(offsetof(NaniteMaterialRecord, albedoTexture) == 0u);
    CHECK(offsetof(NaniteMaterialRecord, normalTexture) == 4u);

    const NaniteMaterialRecord material = NanitePackMaterial(0xDEADBEEFu, 0x12345678u);
    CHECK(material.albedoTexture == 0xDEADBEEFu);
    CHECK(material.normalTexture == 0x12345678u);
    CHECK(NaniteUnpackMaterial(material, 0u) == 0xDEADBEEFu);
    CHECK(NaniteUnpackMaterial(material, 1u) == 0x12345678u);
    CHECK(NaniteUnpackMaterial(material, 2u) == 0u);    // 越界 word
    CHECK(NaniteUnpackMaterial(material, 0xFFFFFFFFu) == 0u);

    // 字节序：word0 = albedo、word1 = normal（与 Slang 侧的 `uint2` 视角逐位一致）
    const u32 words[2] = { 0xA1B2C3D4u, 0x01020304u };
    NaniteMaterialRecord fromWords{};
    std::memcpy(&fromWords, words, sizeof(fromWords));
    CHECK(fromWords.albedoTexture == 0xA1B2C3D4u);
    CHECK(fromWords.normalTexture == 0x01020304u);
    CHECK(NaniteUnpackMaterial(fromWords, 0u) == 0xA1B2C3D4u);

    // 默认记录（0 ⇒ 该槽未绑定；具体语义属任务 12/19 的材质解析）
    const NaniteMaterialRecord empty{};
    CHECK(empty.albedoTexture == 0u);
    CHECK(empty.normalTexture == 0u);
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

    // 任务 10：索引**必须 3 个一组**（3×u16 打包的结构性前提；`indexCount` 是索引总数）
    CHECK(IsNaniteTriangleIndexCount(0u));
    CHECK(IsNaniteTriangleIndexCount(3u));
    CHECK(IsNaniteTriangleIndexCount(30u));
    CHECK_FALSE(IsNaniteTriangleIndexCount(1u));
    CHECK_FALSE(IsNaniteTriangleIndexCount(2u));
    CHECK_FALSE(IsNaniteTriangleIndexCount(4u));
    CHECK_FALSE(IsNaniteTriangleIndexCount(31u));

    // 3×u16 打包的边界：簇内局部下标的实际上限 127 与 u16 上限 255/0xFFFF 的区分
    const NanitePackedTriangle boundary = NanitePackTriangle(0u, 127u, 255u);
    CHECK(NaniteTriangleIndex0(boundary) == 0u);
    CHECK(NaniteTriangleIndex1(boundary) == 127u);
    CHECK(NaniteTriangleIndex2(boundary) == 255u);
    CHECK(IsValidClusterLocalVertexIndex(0u));
    CHECK(IsValidClusterLocalVertexIndex(127u));
    CHECK_FALSE(IsValidClusterLocalVertexIndex(128u));
    CHECK_FALSE(IsValidClusterLocalVertexIndex(255u));   // u16 表示得下，但**语义**非法（>127）
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
// 15b. 任务 11：段对齐原语与 LOD 段步长边界（"偏移"验收的原语层）
//
// 【为什么补这一条（任务 11 收口）】任务 11 的验收是"尺寸 / 偏移 / 量化往返 / 边界全绿"。
//   段偏移的**所有**数字都由 `NaniteAlignUpFile` 产生，而它此前只被 `TryBuildNaniteFileLayout`
//   间接使用、没有被直接钉过边界；`kNaniteLodOffsetBytes`（每个 LOD 一个 u32 的步长）也只在
//   段表用例里被间接推算。这里把这两个"偏移契约的原语"的边界（0 / 1 / 半对齐 / 恰好对齐 /
//   对齐 + 1 / 大值）与 LOD 段的取整行为逐点钉住 —— 与上面 `TryBuildNaniteFileLayout` 的属性
//   循环互补：那边测"推导结果"，这边测"产生结果的原语本身"。
// ============================================================
TEST_CASE("NaniteTypes: 段对齐原语与 LOD 段步长边界（任务 11 偏移验收）") {
    // ① 对齐原语：恰好对齐不动、不足一律向上取整到 16B、0 仍是 0（空段的段长必须是 0）
    CHECK(NaniteAlignUpFile(0u)  == 0u);
    CHECK(NaniteAlignUpFile(1u)  == 16u);
    CHECK(NaniteAlignUpFile(15u) == 16u);
    CHECK(NaniteAlignUpFile(16u) == 16u);
    CHECK(NaniteAlignUpFile(17u) == 32u);
    CHECK(NaniteAlignUpFile(31u) == 32u);
    CHECK(NaniteAlignUpFile(32u) == 32u);
    CHECK(NaniteAlignUpFile(33u) == 48u);
    CHECK(NaniteAlignUpFile(kNaniteFileAlignment) == (u64)kNaniteFileAlignment);

    // 属性循环：任何输入都得到 16B 的倍数、不截断、且是**最小**的那个对齐值（增量 < 一个步长）
    for (u64 v = 0; v <= 128u; ++v) {
        const u64 aligned = NaniteAlignUpFile(v);
        CHECK(aligned % (u64)kNaniteFileAlignment == 0u);
        CHECK(aligned >= v);
        CHECK(aligned - v < (u64)kNaniteFileAlignment);
    }

    // 大值不溢出（u64 内）：u32 计数上限 × 64B 的簇段仍能正常对齐
    const u64 huge = (u64)0xFFFFFFFFu * (u64)kNaniteClusterRecordBytes;
    CHECK(NaniteAlignUpFile(huge) >= huge);
    CHECK(NaniteAlignUpFile(huge) % (u64)kNaniteFileAlignment == 0u);

    // ② LOD 段的步长（任务 7 定稿：每级一个 u32 = 4B）；段的取整由它决定 ⇒ 直接钉住这个尺寸
    CHECK(kNaniteLodOffsetBytes == 4u);

    // ③ LOD 段边界：0 级 ⇒ 0 字节（空资产没有 LOD 段）；1~4 级恰好取整到 16B；5 级 ⇒ 20 → 32B
    const u32   lodCounts[6] = { 0u, 1u, 2u, 4u, 5u, 8u };
    const usize expected[6]  = { 0u, 16u, 16u, 16u, 32u, 32u };
    for (u32 i = 0; i < 6u; ++i) {
        NaniteFileHeader probe{};
        probe.lodLevelCount = lodCounts[i];
        NaniteFileLayout layout{};
        REQUIRE(TryBuildNaniteFileLayout(probe, layout) == NaniteFileError::None);
        CHECK(layout.lodBytes == expected[i]);
        CHECK(layout.totalBytes == layout.lodOffset + layout.lodBytes);
    }
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

// ============================================================
// 17. 任务 13：实例表 128B 镜像（GPUSceneObject 契约）+ 包围球 16B
// ============================================================
TEST_CASE("NaniteTypes: 实例表 128B 镜像（GPUSceneObject 契约）与包围球 16B") {
    // 逐字段偏移必须与 Pipeline/GPUScene.h:26-40 的 GPUSceneObject 一致
    // （跨翻译单元的交叉 static_assert 在 NaniteCull.cpp；这里是同一契约的单测钉子）
    CHECK(sizeof(NaniteInstanceGpuObject) == 128u);
    CHECK(alignof(NaniteInstanceGpuObject) == 16u);   // float4x4 的对齐要求 ⇒ 复现 64/80 偏移
    CHECK(offsetof(NaniteInstanceGpuObject, localToWorld)   == 0u);
    CHECK(offsetof(NaniteInstanceGpuObject, boundsMin)      == 64u);
    CHECK(offsetof(NaniteInstanceGpuObject, boundsMax)      == 80u);
    CHECK(offsetof(NaniteInstanceGpuObject, meshIndex)      == 96u);
    CHECK(offsetof(NaniteInstanceGpuObject, materialIndex)  == 100u);
    CHECK(offsetof(NaniteInstanceGpuObject, objectID)       == 104u);
    CHECK(offsetof(NaniteInstanceGpuObject, visibilityFlags)== 108u);
    CHECK(offsetof(NaniteInstanceGpuObject, indexCount)     == 112u);
    CHECK(offsetof(NaniteInstanceGpuObject, firstIndex)     == 116u);
    CHECK(offsetof(NaniteInstanceGpuObject, vertexOffset)   == 120u);
    CHECK(offsetof(NaniteInstanceGpuObject, _pad)           == 124u);

    // 包围球：16B（StructuredBuffer 步长）；球心 0..2、半径在 12
    CHECK(sizeof(NaniteInstanceSphere) == 16u);
    CHECK(offsetof(NaniteInstanceSphere, center) == 0u);
    CHECK(offsetof(NaniteInstanceSphere, radius) == 12u);

    // 合成实例网格的容量与默认值：默认必须落在容量内（生成侧据此 resize）
    CHECK(kNaniteMaxTestInstances == 256u);
    CHECK(kNaniteDefaultTestInstances > 0u);
    CHECK(kNaniteDefaultTestInstances <= kNaniteMaxTestInstances);
}

// ============================================================
// 18. 任务 13：包围球由 128B 的 boundsMin/boundsMax 推导（含退化 AABB）
// ============================================================
TEST_CASE("NaniteTypes: 包围球由 128B 的 boundsMin/boundsMax 推导") {
    NaniteInstanceGpuObject instance{};
    instance.boundsMin[0] = -1.0f; instance.boundsMin[1] = -2.0f; instance.boundsMin[2] = -2.0f;
    instance.boundsMax[0] =  3.0f; instance.boundsMax[1] =  2.0f; instance.boundsMax[2] =  2.0f;

    const NaniteInstanceSphere sphere = NaniteSphereFromInstanceBounds(instance);
    CHECK(sphere.center[0] == doctest::Approx(1.0f));
    CHECK(sphere.center[1] == doctest::Approx(0.0f));
    CHECK(sphere.center[2] == doctest::Approx(0.0f));
    // 半轴长 (2,2,2) ⇒ 半对角线长 = sqrt(12)
    CHECK(sphere.radius == doctest::Approx(std::sqrt(12.0f)));

    // 退化 AABB（max < min）：半轴长按 0 处理 ⇒ 半径 0（不产生负半径/NaN），球心仍取盒心
    NaniteInstanceGpuObject degenerate{};
    degenerate.boundsMin[0] = 5.0f; degenerate.boundsMax[0] = 1.0f;   // 反向
    degenerate.boundsMin[1] = 2.0f; degenerate.boundsMax[1] = 2.0f;   // 零尺寸
    degenerate.boundsMin[2] = -2.0f; degenerate.boundsMax[2] = -2.0f; // 零尺寸
    const NaniteInstanceSphere degenerateSphere = NaniteSphereFromInstanceBounds(degenerate);
    CHECK(degenerateSphere.radius == doctest::Approx(0.0f));
    CHECK(degenerateSphere.center[0] == doctest::Approx(3.0f));   // (5+1)/2
}

// ============================================================
// 19. 任务 13：视锥六平面提取与 he::Frustum::FromViewProj 逐平面一致
// ============================================================
TEST_CASE("NaniteTypes: 视锥提取（列主序 viewProj）与 he::Frustum 同值") {
    // 一套真实形状的 view-proj：Vulkan [0,1] 深度 + 右手 lookAt（与 CameraData 同构造）
    const float4x4 proj = glm::perspectiveRH_ZO(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    const float4x4 view = glm::lookAtRH(float3(3.0f, 4.0f, 5.0f),
                                        float3(0.0f, 0.0f, 0.0f),
                                        float3(0.0f, 1.0f, 0.0f));
    const float4x4 viewProj = proj * view;

    const NaniteFrustumPlanes extracted = NaniteExtractFrustumPlanes(&viewProj[0][0]);
    const he::Frustum reference = he::Frustum::FromViewProj(viewProj);

    for (u32 i = 0; i < 6u; ++i) {
        for (u32 c = 0; c < 4u; ++c) {
            CHECK(extracted.planes[i][c] == doctest::Approx(reference.planes[i][c]).epsilon(1e-6));
        }
    }

    // 可见性判据也必须同口径：取一批明显不在边界上的球逐个比对
    const NaniteInstanceSphere samples[] = {
        { {  0.0f,  0.0f,   0.0f }, 0.5f },   // 视锥内
        { {  0.0f,  0.0f,  -3.0f }, 0.5f },   // 视锥内（相机看向原点）
        { { 50.0f,  0.0f,   0.0f }, 0.5f },   // 远在右侧外
        { {  0.0f, 50.0f,   0.0f }, 0.5f },   // 远在上方外
        { {  0.0f,  0.0f,  60.0f }, 0.5f },   // 相机背后
        { {  0.0f,  0.0f,   0.0f }, 0.0f },   // 退化成点（仍在内部）
    };
    for (const NaniteInstanceSphere& s : samples) {
        const bool naniteVisible = NaniteSphereVisibleInFrustum(extracted, s.center, s.radius);
        const bool referenceVisible = reference.Intersects(he::Sphere(
            float3(s.center[0], s.center[1], s.center[2]), s.radius));
        CHECK(naniteVisible == referenceVisible);
    }
}

// ============================================================
// 20. 任务 13：CPU 参考实例剔除的已知进/出用例
//
// 用一个**盒状视锥**（[-1,1]^3，平面法线朝内）把判据钉死，不引入矩阵/相机的间接性。
// ============================================================
TEST_CASE("NaniteTypes: CPU 参考实例剔除的已知进/出用例") {
    // 盒 [-1,1]^3：inside 判据 dot(n,p)+d >= 0
    const auto makeBoxFrustum = [] {
        NaniteFrustumPlanes f{};
        const float planes[6][4] = {
            {  1.0f,  0.0f,  0.0f, 1.0f },   // 左：  x >= -1
            { -1.0f,  0.0f,  0.0f, 1.0f },   // 右：  x <=  1
            {  0.0f,  1.0f,  0.0f, 1.0f },   // 下：  y >= -1
            {  0.0f, -1.0f,  0.0f, 1.0f },   // 上：  y <=  1
            {  0.0f,  0.0f,  1.0f, 1.0f },   // 近：  z >= -1
            {  0.0f,  0.0f, -1.0f, 1.0f },   // 远：  z <=  1
        };
        std::memcpy(f.planes, planes, sizeof(planes));
        return f;
    };
    const NaniteFrustumPlanes frustum = makeBoxFrustum();

    // 8 个样本：0/1/2 可见（含跨平面），3/4 不可见，5 空实例，6 退化半径在外，7 退化半径在面上
    struct Case { const char* name; float center[3]; float radius; u32 indexCount; bool visible; };
    const Case cases[] = {
        { "完全在内部",       {  0.0f, 0.0f, 0.0f }, 0.25f, 36u, true  },
        { "完全在内部（near）",{  0.9f,-0.9f, 0.9f }, 0.05f, 36u, true  },
        { "跨越右平面（可见）",{  1.05f,0.0f, 0.0f }, 0.10f, 36u, true  },
        { "完全在右平面外",   {  1.20f,0.0f, 0.0f }, 0.10f, 36u, false },
        { "完全在上方外",     {  0.0f, 5.0f, 0.0f }, 0.50f, 36u, false },
        { "空实例（indexCount=0）", { 0.0f, 0.0f, 0.0f }, 0.25f, 0u, false },
        { "退化半径在外",     {  1.50f,0.0f, 0.0f }, 0.00f, 36u, false },
        { "退化半径在面上",   {  1.00f,0.0f, 0.0f }, 0.00f, 36u, true  },   // dist == 0，判据是 < -r
    };
    const u32 caseCount = (u32)(sizeof(cases) / sizeof(cases[0]));

    std::vector<NaniteInstanceGpuObject> instances(caseCount);
    std::vector<NaniteInstanceSphere>    spheres(caseCount);
    std::vector<u32>                     expected;
    for (u32 i = 0; i < caseCount; ++i) {
        instances[i] = NaniteInstanceGpuObject{};
        instances[i].indexCount = cases[i].indexCount;
        spheres[i].center[0] = cases[i].center[0];
        spheres[i].center[1] = cases[i].center[1];
        spheres[i].center[2] = cases[i].center[2];
        spheres[i].radius    = cases[i].radius;
        if (cases[i].visible) expected.push_back(i);
    }

    std::vector<u32> visible(caseCount, 0xFFFFFFFFu);
    const u32 count = NaniteCullInstancesCPU(frustum, instances.data(), spheres.data(),
                                             caseCount, visible.data(), (u32)visible.size());
    REQUIRE(count == (u32)expected.size());
    for (u32 i = 0; i < count; ++i) {
        CHECK(visible[i] == expected[i]);   // 升序紧凑
    }

    // 把"哪些样本进、哪些出"打成 MESSAGE：验收要的是一眼可核对的读数，而不是只看绿灯
    {
        std::string list;
        for (u32 i = 0; i < count; ++i) {
            if (!list.empty()) list += ",";
            list += std::to_string(visible[i]);
        }
        const std::string msg =
            "CPU 参考实例剔除（盒 [-1,1]^3）：8 样本 → 可见 " + std::to_string(count)
            + " 个，升序下标 [" + list + "]；进=0/1/2/7（2 跨右平面、7 退化半径恰在面上），"
            "出=3/4/6、空实例 5 因 indexCount=0 跳过";
        MESSAGE(msg);
    }

    // 逐条再验一次判据本身（失败时能直接指出是哪一类样本）
    for (u32 i = 0; i < caseCount; ++i) {
        const bool got = NaniteSphereVisibleInFrustum(frustum, spheres[i].center, spheres[i].radius);
        if (cases[i].indexCount == 0u) continue;   // 空实例不参与判据（由 CPU 剔除的跳过规则处理）
        CHECK(got == cases[i].visible);
    }
}

// ============================================================
// 21. 任务 13：CPU 参考剔除的边界（空表 / 空指针 / 容量截断 / 平面数完整）
// ============================================================
TEST_CASE("NaniteTypes: CPU 参考实例剔除的空表与容量截断") {
    NaniteFrustumPlanes frustum{};
    // 恒可见的退化视锥（6 个平面法线为 0、d = 0 ⇒ dot+d = 0 >= -r 恒真）
    std::vector<NaniteInstanceGpuObject> instances(4);
    std::vector<NaniteInstanceSphere>    spheres(4, NaniteInstanceSphere{ { 0.0f, 0.0f, 0.0f }, 0.0f });
    std::vector<u32>                     out(4, 0xFFFFFFFFu);

    // 空表：0 条 ⇒ 0 个可见
    CHECK(NaniteCullInstancesCPU(frustum, instances.data(), spheres.data(), 0u, out.data(), 4u) == 0u);

    // 空指针：任一输入为 null ⇒ 0（不崩）
    CHECK(NaniteCullInstancesCPU(frustum, nullptr, spheres.data(), 4u, out.data(), 4u) == 0u);
    CHECK(NaniteCullInstancesCPU(frustum, instances.data(), nullptr, 4u, out.data(), 4u) == 0u);
    CHECK(NaniteCullInstancesCPU(frustum, instances.data(), spheres.data(), 4u, nullptr, 4u) == 0u);
    CHECK(NaniteCullInstancesCPU(frustum, instances.data(), spheres.data(), 4u, out.data(), 0u) == 0u);

    // 容量截断：4 条全可见、容量 2 ⇒ 返回 2，且是升序的前两条
    for (u32 i = 0; i < 4u; ++i) instances[i].indexCount = 36u;
    CHECK(NaniteCullInstancesCPU(frustum, instances.data(), spheres.data(), 4u, out.data(), 2u) == 2u);
    CHECK(out[0] == 0u);
    CHECK(out[1] == 1u);

    // 容量恰好等于可见数：4 条 ⇒ 4；全部可见且升序
    const u32 all = NaniteCullInstancesCPU(frustum, instances.data(), spheres.data(), 4u, out.data(), 4u);
    REQUIRE(all == 4u);
    for (u32 i = 0; i < 4u; ++i) CHECK(out[i] == i);

    // 6 个平面缺一不可：只把"右平面"改成把整个盒推到外侧，样本立刻不可见
    // （防止实现只检查前 5 个平面之类的复制粘贴缺陷）
    NaniteFrustumPlanes clipped = frustum;
    clipped.planes[1][0] = -1.0f;   // 右：-x + 1 >= 0 ⇒ x <= 1
    clipped.planes[1][1] = 0.0f;
    clipped.planes[1][2] = 0.0f;
    clipped.planes[1][3] = -100.0f; // x <= -100 ⇒ 原点在右侧外
    CHECK_FALSE(NaniteSphereVisibleInFrustum(clipped, spheres[0].center, spheres[0].radius));
}
