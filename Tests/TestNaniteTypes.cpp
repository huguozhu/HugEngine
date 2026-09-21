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
//
// §14.8 任务 14（per-instance cluster BVH）追加的用例（本文件只放"布局契约 + 参考遍历"，
//   构建器的节点数/深度/复现性在 `TestNaniteBuilder.cpp`）：
//  22. 节点 32B / 簇球 16B / 引用 8B 的布局与容量常量；`NaniteBVHNodeIsLeaf` 判据
//  23. CPU 参考遍历：空表与空指针、单簇、手搭 7 节点树的访问数（全部在内/全部在外/部分相交/
//      内部节点剪枝）、多实例与实例域钳制、栈溢出防御（计数器不静默）
//  24. 视图与读数仍是纯 POD（可被 Scene 侧 include 的前提）
//
// §14.8 任务 15（三阶段簇剔除）追加的用例：
//  25a. Hi-Z 金字塔层数公式（与 GPUCulling::BuildHiZPyramid 同式）与可采样层下限
//  25b. 球 → 屏幕 AABB + 最近深度（含相机之后 / 空指针 / 退化球）
//  25c. Hi-Z 选层公式与边界（NaN/负尺寸/层数不足）
//  25d. 遮挡判据与**开关两档**（关闭恒不遮挡且 0 次采样；打开时按最近深度 > 采样值判定）
//  25e. LOD 选择的**单调性**（距离越远级别越粗）+ 链上恰好选中一级 + 关闭档不筛
//  25f. 三阶段参考遍历：Phase 1 掩码、视锥、Hi-Z、LOD 级分布、复现性、空表/单簇/退化球边界
//  25g. 三阶段参数与元数据的布局契约（GPU 侧结构体逐字段对应）
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
#include <type_traits>  // 【任务 14】is_trivially_copyable（视图结构体仍是纯 POD）
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
// 12e. 任务 19：材质记录 32B 打包/解包（由任务 7/10 的 8B **最小扩展**而来）
//   §8/任务 7 的 8B 记录只有两个 bindless 纹理 ID，放不下"因子 + 基础色 + 纹理掩码"这三样
//   GBuffer 路径逐项对照所需的字段 ⇒ 扩到 32B（字段语义见 `NaniteTypes.h`）。
// ============================================================
TEST_CASE("NaniteTypes: 材质记录 32B 打包/解包（任务 19 最小扩展）") {
    static_assert(sizeof(NaniteMaterialRecord) == 32, "材质记录必须 32B（任务 19 最小扩展）");
    CHECK(kNaniteMaterialRecordBytes == 32u);
    CHECK(offsetof(NaniteMaterialRecord, baseColorFactor)     == 0u);
    CHECK(offsetof(NaniteMaterialRecord, metallicFactor)      == 16u);
    CHECK(offsetof(NaniteMaterialRecord, roughnessFactor)     == 20u);
    CHECK(offsetof(NaniteMaterialRecord, textureMask)         == 24u);
    CHECK(offsetof(NaniteMaterialRecord, bindlessTextureBase) == 28u);

    const float factor[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
    const NaniteMaterialRecord material = NaniteMakeMaterialRecord(factor, 0.125f, 0.875f, 0x7u, 0xDEADBEEFu);
    CHECK(material.baseColorFactor[0] == 0.25f);
    CHECK(material.baseColorFactor[3] == 1.0f);
    CHECK(material.metallicFactor == 0.125f);
    CHECK(material.roughnessFactor == 0.875f);
    CHECK(material.textureMask == 0x7u);
    CHECK(material.bindlessTextureBase == 0xDEADBEEFu);

    // 原始字视图（word 0..7 = 8×u32）：偏移与 Slang 侧 `StructuredBuffer` 视角逐位对应
    CHECK(NaniteUnpackMaterial(material, 0u) == 0x3E800000u);   // 0.25f 的位模式
    CHECK(NaniteUnpackMaterial(material, 4u) == 0x3E000000u);   // 0.125f
    CHECK(NaniteUnpackMaterial(material, 5u) == 0x3F600000u);   // 0.875f
    CHECK(NaniteUnpackMaterial(material, 6u) == 0x7u);
    CHECK(NaniteUnpackMaterial(material, 7u) == 0xDEADBEEFu);
    CHECK(NaniteUnpackMaterial(material, 8u) == 0u);            // 越界 word
    CHECK(NaniteUnpackMaterial(material, 0xFFFFFFFFu) == 0u);

    // 默认记录：glTF 的中性默认（基础色 1、金属度 1、粗糙度 1、无纹理掩码）
    const NaniteMaterialRecord empty{};
    CHECK(empty.baseColorFactor[0] == 1.0f);
    CHECK(empty.metallicFactor == 1.0f);
    CHECK(empty.roughnessFactor == 1.0f);
    CHECK(empty.textureMask == 0u);
    CHECK(empty.bindlessTextureBase == 0u);

    // 空指针兜底：因子退化为 1（不崩）
    const NaniteMaterialRecord fallback = NaniteMakeMaterialRecord(nullptr, 0.0f, 0.5f, 0u, 3u);
    CHECK(fallback.baseColorFactor[0] == 1.0f);
    CHECK(fallback.baseColorFactor[3] == 1.0f);
    // 【簇 → 源网格 → 材质 的映射规则由 `Tests/TestNaniteBuilder.cpp` 覆盖】
    //   （`NaniteAssignClusterMaterials` 定义在 `NaniteUpload.cpp`，本文件只编格式、不链接它）
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
    CHECK(layout.materialBytes  == 2u * 32u);     // 任务 19：材质记录 32B（2×32 = 64B，已是 16 的倍数）
    CHECK(layout.lodOffset      == 528u + 64u);
    CHECK(layout.lodBytes       == 16u);          // 1 × 4B 向上取整到 16B
    CHECK(layout.totalBytes     == 592u + 16u);
    CHECK(layout.triangleCount  == 10u);
    CHECK(layout.totalBytes % kNaniteFileAlignment == 0u);

    // 属性循环：任意计数下每个段的起点都 16B 对齐（⇒ `Misaligned` 分支不可达），且段首尾相接
    const u32 counts[] = { 0u, 1u, 3u, 7u, 64u, 1001u };
    for (u32 clusterCount : counts) {
        for (u32 vertexCount : counts) {
            NaniteFileHeader probe{};
            probe.clusterCount  = clusterCount;
            probe.vertexCount   = vertexCount;
            probe.materialCount = clusterCount;    // 让材质段随簇数变化（32B 步长的对齐压力）
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
    REQUIRE(layout.totalBytes == 608u);   // 任务 19：材质段 2×32 = 64B（原 8B 记录时为 560u）
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

// ============================================================
// 22. 任务 14：cluster BVH 的布局契约 + CPU 参考深度优先遍历（手搭树，RHI-free）
//
// 【为什么用手搭树而不是构建器】`NaniteTypes.h` 里的参考遍历必须能被**独立**验证：构建器在
//   `NaniteUpload.cpp`（另一个翻译单元）。手搭树能把"访问数"精确钉在已知结构上，
//   而构建器的节点数/深度/复现性由 `TestNaniteBuilder.cpp` 的 `NaniteBVH:` 用例覆盖。
// ============================================================

namespace {

/// 盒状视锥 [-half, half]^3（与任务 13 用例同一口径）
NaniteFrustumPlanes MakeBVHBoxFrustum(float halfExtent) {
    NaniteFrustumPlanes frustum{};
    const float planes[6][4] = {
        {  1.0f, 0.0f, 0.0f, halfExtent },
        { -1.0f, 0.0f, 0.0f, halfExtent },
        {  0.0f, 1.0f, 0.0f, halfExtent },
        {  0.0f,-1.0f, 0.0f, halfExtent },
        {  0.0f, 0.0f, 1.0f, halfExtent },
        {  0.0f, 0.0f,-1.0f, halfExtent },
    };
    std::memcpy(frustum.planes, planes, sizeof(planes));
    return frustum;
}

/// 手工构造一个叶子节点
NaniteBVHNode MakeLeafNode(float cx, float cy, float cz, float radius,
                           u32 firstCluster, u32 clusterCount) {
    NaniteBVHNode node{};
    node.center[0] = cx;
    node.center[1] = cy;
    node.center[2] = cz;
    node.radius    = radius;
    node.left      = firstCluster;
    node.right     = kNaniteBVHNoChild;
    node.count     = clusterCount;
    node.flags     = kNaniteBVHNodeFlagLeaf;
    return node;
}

/// 手工构造一个内部节点
NaniteBVHNode MakeInnerNode(float cx, float cy, float cz, float radius, u32 left, u32 right) {
    NaniteBVHNode node{};
    node.center[0] = cx;
    node.center[1] = cy;
    node.center[2] = cz;
    node.radius    = radius;
    node.left      = left;
    node.right     = right;
    node.count     = 2u;
    node.flags     = 0u;
    return node;
}

/// 只带平移与 indexCount 的合成实例（平移写在列主序 localToWorld 的第 4 列）
NaniteInstanceGpuObject MakeBVHTranslatedInstance(float x, float y, float z, u32 indexCount) {
    NaniteInstanceGpuObject instance{};
    instance.localToWorld[0]  = 1.0f;
    instance.localToWorld[5]  = 1.0f;
    instance.localToWorld[10] = 1.0f;
    instance.localToWorld[15] = 1.0f;
    instance.localToWorld[12] = x;
    instance.localToWorld[13] = y;
    instance.localToWorld[14] = z;
    instance.indexCount = indexCount;
    return instance;
}

} // namespace

TEST_CASE("NaniteTypes: cluster BVH 的 POD 布局契约（32B 节点 / 16B 球 / 8B 引用）") {
    // 【契约】这三个布局是 GPU 与 CPU 共享的那一份；任何一边漂移都会让"逐项一致"失去意义。
    // `Nanite_ClusterBVH.comp.slang` 侧写成 `float4 centerRadius + uint4 link` 与 `float4`，
    // 与下面逐字段 pin 住的偏移一一对应。
    CHECK(sizeof(NaniteBVHNode) == 32u);
    CHECK(offsetof(NaniteBVHNode, center) == 0u);
    CHECK(offsetof(NaniteBVHNode, radius) == 12u);
    CHECK(offsetof(NaniteBVHNode, left)   == 16u);
    CHECK(offsetof(NaniteBVHNode, right)  == 20u);
    CHECK(offsetof(NaniteBVHNode, count)  == 24u);
    CHECK(offsetof(NaniteBVHNode, flags)  == 28u);

    CHECK(sizeof(NaniteClusterSphere) == 16u);
    CHECK(offsetof(NaniteClusterSphere, center) == 0u);
    CHECK(offsetof(NaniteClusterSphere, radius) == 12u);

    CHECK(sizeof(NaniteVisibleClusterRef) == 8u);
    CHECK(offsetof(NaniteVisibleClusterRef, instance) == 0u);
    CHECK(offsetof(NaniteVisibleClusterRef, cluster)  == 4u);

    // 叶子位判据与常量
    CHECK(kNaniteBVHNodeFlagLeaf == 1u);
    CHECK(kNaniteBVHLeafCapacity == 4u);
    CHECK(kNaniteBVHMaxDepth == 24u);
    CHECK(kNaniteBVHMaxStackDepth >= kNaniteBVHMaxDepth);
    CHECK(kNaniteBVHMaxStackDepth == 32u);

    // 容量常量之间的关系：节点上界 = 2 × 簇数 - 1；可见引用容量 = 实例域 × 簇数上限
    CHECK(kNaniteMaxBVHNodes == 2u * kNaniteMaxBVHClusters);
    CHECK(kNaniteMaxVisibleClusterRefs == kNaniteMaxBVHInstances * kNaniteMaxBVHClusters);

    NaniteBVHNode leaf = MakeLeafNode(1.0f, 2.0f, 3.0f, 0.5f, 7u, 3u);
    CHECK(NaniteBVHNodeIsLeaf(leaf));
    leaf.flags = 0u;
    CHECK_FALSE(NaniteBVHNodeIsLeaf(leaf));
    CHECK(kNaniteBVHNoChild == 0xFFFFFFFFu);
}

TEST_CASE("NaniteTypes: CPU 参考 cluster BVH 遍历的空表与空指针") {
    const NaniteFrustumPlanes frustum = MakeBVHBoxFrustum(4.0f);
    const NaniteInstanceGpuObject instance = MakeBVHTranslatedInstance(0.0f, 0.0f, 0.0f, 36u);
    NaniteVisibleClusterRef out[4] = {};
    NaniteClusterBVHTraversalStats stats{};

    const NaniteBVHNode         nodes[1]   = { MakeLeafNode(0.0f, 0.0f, 0.0f, 1.0f, 0u, 1u) };
    const u32                   leaves[1]  = { 0u };
    const NaniteClusterSphere   spheres[1] = { { { 0.0f, 0.0f, 0.0f }, 1.0f } };

    NaniteClusterBVHView view;
    view.nodes              = nodes;
    view.nodeCount          = 1u;
    view.leafClusterIndices = leaves;
    view.clusterSpheres     = spheres;
    view.clusterCount       = 1u;

    // 空 BVH：节点表空 / 节点数 0 ⇒ 0（stats 必须被清零，不能留上一次的读数）
    stats.visitedNodes = 123u;
    CHECK(NaniteTraverseClusterBVHCPU(frustum, NaniteClusterBVHView{}, &instance, 1u, 4u,
                                      out, 4u, &stats) == 0u);
    CHECK(stats.visitedNodes == 0u);
    CHECK(stats.visibleClusters == 0u);

    NaniteClusterBVHView nodeCountZero = view;
    nodeCountZero.nodeCount = 0u;
    CHECK(NaniteTraverseClusterBVHCPU(frustum, nodeCountZero, &instance, 1u, 4u,
                                      out, 4u, &stats) == 0u);

    // 空实例表 / 0 实例 / 0 域 ⇒ 0（不崩）
    CHECK(NaniteTraverseClusterBVHCPU(frustum, view, nullptr, 1u, 4u, out, 4u, &stats) == 0u);
    CHECK(NaniteTraverseClusterBVHCPU(frustum, view, &instance, 0u, 4u, out, 4u, &stats) == 0u);
    CHECK(NaniteTraverseClusterBVHCPU(frustum, view, &instance, 1u, 0u, out, 4u, &stats) == 0u);

    // 正常的最小情形（1 节点 / 1 簇）：进、出各一例
    CHECK(NaniteTraverseClusterBVHCPU(frustum, view, &instance, 1u, 4u, out, 4u, &stats) == 1u);
    CHECK(stats.visitedNodes == 1u);
    CHECK(stats.traversedInstances == 1u);
    CHECK(out[0].instance == 0u);
    CHECK(out[0].cluster == 0u);

    const NaniteInstanceGpuObject farInstance = MakeBVHTranslatedInstance(50.0f, 0.0f, 0.0f, 36u);
    CHECK(NaniteTraverseClusterBVHCPU(frustum, view, &farInstance, 1u, 4u, out, 4u, &stats) == 0u);
    CHECK(stats.visitedNodes == 1u);      // 根仍被访问一次
    CHECK(stats.visibleClusters == 0u);
}

TEST_CASE("NaniteTypes: CPU 参考 cluster BVH 遍历（手搭 7 节点树的访问数）") {
    // 手搭一棵 7 节点满二叉树，4 个叶子各放 1 个簇：
    //   node0 = 内部（左 node1 / 右 node2）
    //   node1 = 内部（左 node3 / 右 node4）—— 簇 0、1 在 x = -3、-1
    //   node2 = 内部（左 node5 / 右 node6）—— 簇 2、3 在 x = +1、+3
    // 盒视锥 [-4,4]^3、簇球半径 0.5、实例为纯平移 ⇒ 每类情形的访问数可解析算出：
    //   · 实例在原点、盒 [-4,4]：全部可见 ⇒ 访问 7、可见 4
    //   · 实例在原点、盒 [-1.5,1.5]：只有 node1/node2 子树可见（node3/node6 的簇在外）
    //   · 实例在 x=100：只有根被访问 ⇒ 访问 1、可见 0
    std::vector<NaniteBVHNode> nodes(7u);
    nodes[0] = MakeInnerNode( 0.0f, 0.0f, 0.0f, 3.5f, 1u, 2u);
    nodes[1] = MakeInnerNode(-2.0f, 0.0f, 0.0f, 1.5f, 3u, 4u);
    nodes[2] = MakeInnerNode( 2.0f, 0.0f, 0.0f, 1.5f, 5u, 6u);
    nodes[3] = MakeLeafNode(-3.0f, 0.0f, 0.0f, 0.5f, 0u, 1u);
    nodes[4] = MakeLeafNode(-1.0f, 0.0f, 0.0f, 0.5f, 1u, 1u);
    nodes[5] = MakeLeafNode( 1.0f, 0.0f, 0.0f, 0.5f, 2u, 1u);
    nodes[6] = MakeLeafNode( 3.0f, 0.0f, 0.0f, 0.5f, 3u, 1u);

    const u32 leaves[4] = { 0u, 1u, 2u, 3u };
    const NaniteClusterSphere spheres[4] = {
        { { -3.0f, 0.0f, 0.0f }, 0.5f },
        { { -1.0f, 0.0f, 0.0f }, 0.5f },
        { {  1.0f, 0.0f, 0.0f }, 0.5f },
        { {  3.0f, 0.0f, 0.0f }, 0.5f },
    };

    NaniteClusterBVHView view;
    view.nodes              = nodes.data();
    view.nodeCount          = (u32)nodes.size();
    view.leafClusterIndices = leaves;
    view.clusterSpheres     = spheres;
    view.clusterCount       = 4u;

    // ── ① 全部在内：7 个节点全被访问、4 个簇全可见 ──
    {
        const NaniteInstanceGpuObject instance = MakeBVHTranslatedInstance(0.0f, 0.0f, 0.0f, 36u);
        NaniteVisibleClusterRef out[4] = {};
        NaniteClusterBVHTraversalStats stats{};
        const u32 written = NaniteTraverseClusterBVHCPU(MakeBVHBoxFrustum(4.0f), view,
                                                        &instance, 1u, 4u, out, 4u, &stats);
        CHECK(written == 4u);
        CHECK(stats.visitedNodes == 7u);
        CHECK(stats.traversedInstances == 1u);
        CHECK(stats.stackOverflows == 0u);
    }

    // ── ② 全部在外：只访问根 ──
    {
        const NaniteInstanceGpuObject instance = MakeBVHTranslatedInstance(100.0f, 0.0f, 0.0f, 36u);
        NaniteVisibleClusterRef out[4] = {};
        NaniteClusterBVHTraversalStats stats{};
        CHECK(NaniteTraverseClusterBVHCPU(MakeBVHBoxFrustum(4.0f), view,
                                          &instance, 1u, 4u, out, 4u, &stats) == 0u);
        CHECK(stats.visitedNodes == 1u);
    }

    // ── ③ 部分相交：盒 [-1.5,1.5] ⇒ 只有 |x| ≤ 1.5-0.5=1.0 的簇可见（簇 1 与簇 2）；
    //        三个内部节点的球都还在盒内 ⇒ 7 个节点**全部被访问**（早退只发生在内部节点上，
    //        叶子被访问后仍要逐个测簇球）──
    {
        const NaniteInstanceGpuObject instance = MakeBVHTranslatedInstance(0.0f, 0.0f, 0.0f, 36u);
        NaniteVisibleClusterRef out[4] = {};
        NaniteClusterBVHTraversalStats stats{};
        const u32 written = NaniteTraverseClusterBVHCPU(MakeBVHBoxFrustum(1.5f), view,
                                                        &instance, 1u, 4u, out, 4u, &stats);
        CHECK(written == 2u);
        CHECK(stats.visitedNodes == 7u);   // 内部节点都可见 ⇒ 没有子树被剪掉
        std::vector<u32> clusters;
        for (u32 i = 0u; i < written; ++i) clusters.push_back(out[i].cluster);
        std::sort(clusters.begin(), clusters.end());
        REQUIRE(clusters.size() == 2u);
        CHECK(clusters[0] == 1u);
        CHECK(clusters[1] == 2u);
    }

    // ── ③b 剪枝确实发生在内部节点上：盒缩到 [-0.4,0.4] ⇒ 两个子节点球（球心 ±2、半径 1.5）
    //        都不可见（|∓2| - 0.4 = 1.6 > 1.5）⇒ 两棵子树整体跳过，只访问 node0/node1/node2 ──
    {
        const NaniteInstanceGpuObject instance = MakeBVHTranslatedInstance(0.0f, 0.0f, 0.0f, 36u);
        NaniteVisibleClusterRef out[4] = {};
        NaniteClusterBVHTraversalStats stats{};
        CHECK(NaniteTraverseClusterBVHCPU(MakeBVHBoxFrustum(0.4f), view,
                                          &instance, 1u, 4u, out, 4u, &stats) == 0u);
        CHECK(stats.visitedNodes == 3u);   // node0（根） + node1 + node2；4 个叶子全部跳过
    }

    // ── ④ 多实例 + 实例域钳制 + 空实例跳过 ──
    {
        const std::vector<NaniteInstanceGpuObject> all = {
            MakeBVHTranslatedInstance(0.0f, 0.0f, 0.0f, 36u),     // 全在内
            MakeBVHTranslatedInstance(100.0f, 0.0f, 0.0f, 36u),   // 全在外
            MakeBVHTranslatedInstance(0.0f, 0.0f, 0.0f, 0u),      // 空实例
        };
        NaniteVisibleClusterRef out[8] = {};
        NaniteClusterBVHTraversalStats stats{};
        CHECK(NaniteTraverseClusterBVHCPU(MakeBVHBoxFrustum(4.0f), view, all.data(), 3u, 4u,
                                          out, 8u, &stats) == 4u);
        CHECK(stats.visitedNodes == 8u);          // 7（全在内）+ 1（全在外）
        CHECK(stats.traversedInstances == 2u);    // 空实例被跳过

        // 域钳到 1 ⇒ 只算第一个实例
        CHECK(NaniteTraverseClusterBVHCPU(MakeBVHBoxFrustum(4.0f), view, all.data(), 3u, 1u,
                                          out, 8u, &stats) == 4u);
        CHECK(stats.visitedNodes == 7u);
        CHECK(stats.traversedInstances == 1u);
        for (u32 i = 0u; i < 4u; ++i) CHECK(out[i].instance == 0u);
    }
}

TEST_CASE("NaniteTypes: CPU 参考 cluster BVH 遍历的栈溢出防御（构建器不会产出的退化树）") {
    // 【本用例证明什么】`kNaniteBVHMaxDepth` 是构建器的硬不变量（`TestNaniteBuilder.cpp` 断言），
    //   所以 GPU/CPU 的显式栈不会溢出。但参考实现仍必须**不静默、不越界**：这里手搭一棵
    //   "左链 + 共用一个叶子"的 41 层退化树（深度远超 32），要求
    //     ① `stackOverflows > 0`（溢出被计数）；② 不越界、不崩、访问数有限。
    const u32 kChain = 40u;              // 内部节点 0..39
    const u32 kLeafIndex = kChain;       // 节点 40 = 共用叶子
    std::vector<NaniteBVHNode> nodes(kChain + 1u);
    for (u32 i = 0u; i < kChain; ++i) {
        // 每个内部节点：左孩子 = 下一个内部节点（链），右孩子 = 共用的叶子
        // （半径足够大 ⇒ 一路可见，确保 DFS 真的往下走）
        nodes[i] = MakeInnerNode(0.0f, 0.0f, 0.0f, 1.0f, i + 1u, kLeafIndex);
    }
    nodes[kLeafIndex] = MakeLeafNode(0.0f, 0.0f, 0.0f, 0.5f, 0u, 1u);

    const u32 leaves[1] = { 0u };
    const NaniteClusterSphere spheres[1] = { { { 0.0f, 0.0f, 0.0f }, 0.5f } };

    NaniteClusterBVHView view;
    view.nodes              = nodes.data();
    view.nodeCount          = (u32)nodes.size();
    view.leafClusterIndices = leaves;
    view.clusterSpheres     = spheres;
    view.clusterCount       = 1u;

    const NaniteInstanceGpuObject instance = MakeBVHTranslatedInstance(0.0f, 0.0f, 0.0f, 36u);
    std::vector<NaniteVisibleClusterRef> out(256u);
    NaniteClusterBVHTraversalStats stats{};
    const u32 written = NaniteTraverseClusterBVHCPU(MakeBVHBoxFrustum(4.0f), view, &instance,
                                                    1u, 4u, out.data(), (u32)out.size(), &stats);

    CHECK(stats.stackOverflows > 0u);                       // 溢出被计数（不静默）
    CHECK(stats.visitedNodes > 0u);
    CHECK(stats.visitedNodes <= 2u * (kChain + 1u));         // 访问数有限（没有指数级重复遍历）
    CHECK(written == stats.visibleClusters);
    CHECK(written <= 2u * (kChain + 1u));
    MESSAGE("栈溢出防御：链深 " << kChain << "，访问 " << stats.visitedNodes
            << "，溢出 " << stats.stackOverflows << "，可见 " << written);
}

// ============================================================
// 23. 任务 14：BVH 视图结构体仍是纯 POD（可被 Scene 侧 include 的前提）
// ============================================================
TEST_CASE("NaniteTypes: cluster BVH 视图与读数是纯 POD") {
    CHECK(std::is_trivially_copyable<NaniteClusterBVHView>::value);
    CHECK(std::is_trivially_copyable<NaniteClusterBVHTraversalStats>::value);
    CHECK(std::is_trivially_copyable<NaniteBVHNode>::value);
    CHECK(std::is_trivially_copyable<NaniteClusterSphere>::value);
    CHECK(std::is_trivially_copyable<NaniteVisibleClusterRef>::value);

    const NaniteClusterBVHView view{};
    CHECK(view.nodes == nullptr);
    CHECK(view.nodeCount == 0u);
    CHECK(view.leafClusterIndices == nullptr);
    CHECK(view.clusterSpheres == nullptr);
    CHECK(view.clusterCount == 0u);

    const NaniteClusterBVHTraversalStats stats{};
    CHECK(stats.visitedNodes == 0u);
    CHECK(stats.visibleClusters == 0u);
    CHECK(stats.stackOverflows == 0u);
    CHECK(stats.traversedInstances == 0u);
}

// ============================================================
// 25. 任务 15：三阶段簇剔除（Phase 1 掩码 → Phase 2 视锥 + Hi-Z → Phase 3 LOD 选择）
//
// 【本节测什么】判据层（`NaniteTypes.h` 的"任务 15"小节）的**已知进/出/遮挡用例**、
//   Hi-Z 开关两档的差异、LOD 选择的单调性（距离越远级别越粗）、边界（空表/单簇/退化球）、
//   以及"两次调用逐位可复现"。GPU 侧的同名判据在 `Nanite_ClusterBVH.comp.slang`。
// 【Hi-Z 在 CPU 侧怎么做得到】生产路径传空采样器（CPU 拿不到金字塔的逐 texel 内容），
//   但**判据本身**可以用一个"合成金字塔"的回调完整测起来 —— 这正是本节的 `SyntheticHiZ`。
// ============================================================

namespace {

/// 合成 Hi-Z 金字塔：每层返回一个固定深度（用来把"遮挡/不遮挡"两档做成可判定的已知用例）
struct SyntheticHiZ {
    float depthPerMip[kNaniteMaxHiZMips] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    u32   sampleCalls = 0u;
    u32   lastMip     = 0u;
};

bool SyntheticHiZSample(void* user, float /*u*/, float /*v*/, u32 mip, float* outDepth) {
    auto* hiz = static_cast<SyntheticHiZ*>(user);
    if (hiz == nullptr || outDepth == nullptr || mip >= kNaniteMaxHiZMips) return false;
    ++hiz->sampleCalls;
    hiz->lastMip = mip;
    *outDepth = hiz->depthPerMip[mip];
    return true;
}

/// 合成"标准透视投影"的视锥（Vulkan [0,1] 深度、列主序 glm 口径：`m[col*4+row]`）
///
/// 相机在原点、朝 -Z，`view = I` ⇒ 世界点 `(0,0,-d)` 的 ndc.z = f/(f-n) × (1 - n/d)（近 = 0）。
/// 【为什么要自己写而不是用 glm】单测不引入新的矩阵库依赖，也把"列主序"这条口径写死在用例里。
void MakeTestPerspective(float fovYDegrees, float aspect, float nearZ, float farZ, float outM[16]) {
    const float f = 1.0f / std::tan(fovYDegrees * 0.5f * 3.14159265358979323846f / 180.0f);
    for (u32 i = 0u; i < 16u; ++i) outM[i] = 0.0f;
    outM[0]  = f / aspect;                       // m[0][0]
    outM[5]  = f;                                // m[1][1]
    outM[10] = farZ / (nearZ - farZ);            // m[2][2]
    outM[11] = -1.0f;                            // m[2][3]
    outM[14] = -(farZ * nearZ) / (farZ - nearZ); // m[3][2]
}

/// 列主序矩阵 → 4 个"行"（`rows[r*4+c] = m[c*4+r]`），与 `NaniteCull::SetCullChainFrame` 同一口径
void MakeViewProjRows(const float m[16], float outRows[16]) {
    for (u32 row = 0u; row < 4u; ++row) {
        for (u32 col = 0u; col < 4u; ++col) {
            outRows[row * 4u + col] = m[col * 4u + row];
        }
    }
}

/// 合成一条 LOD 链的元数据（级 0 = 最细 … 级 n-1 = 根）：
///   级 L 的 ownError = stepErrors[L-1]（叶子 0）、parentError = stepErrors[L]、根带根标志
void MakeLODChain(const float* stepErrors, u32 stepCount, std::vector<NaniteClusterLODInfo>& out) {
    out.assign(stepCount + 1u, NaniteClusterLODInfo{});
    for (u32 level = 0u; level <= stepCount; ++level) {
        NaniteClusterLODInfo& info = out[level];
        info.lodLevel    = level;
        info.ownError    = (level == 0u) ? 0.0f : stepErrors[level - 1u];
        info.parentError = (level < stepCount) ? stepErrors[level] : 0.0f;
        info.flags       = (level == stepCount) ? kNaniteLODInfoFlagRoot : 0u;
    }
}

/// 在一条 LOD 链上找"被选中的那一级"（正常应恰好一级；找不到返回哨兵）
u32 SelectedLevelOnChain(const std::vector<NaniteClusterLODInfo>& chain,
                         float distance, float focalPixels, float thresholdPixels) {
    for (u32 level = 0u; level < (u32)chain.size(); ++level) {
        if (NaniteLODClusterSelected(chain[level], distance, focalPixels, thresholdPixels)) {
            return level;
        }
    }
    return 0xFFFFFFFFu;
}

} // namespace

// ── 25a. Hi-Z 金字塔层数公式（必须与 GPUCulling::BuildHiZPyramid 的公式一致）──
TEST_CASE("NaniteHiZ: 金字塔层数公式与可采样层下限") {
    CHECK(NaniteHiZPyramidMipCount(1u, 1u) == 1u);      // 单像素：连一层都写不出（<2 尺寸会关闭）
    CHECK(NaniteHiZPyramidMipCount(2u, 1u) == 1u);
    CHECK(NaniteHiZPyramidMipCount(4u, 4u) == 2u);      // 4 → 2
    CHECK(NaniteHiZPyramidMipCount(8u, 8u) == 3u);      // 8 → 4 → 2
    CHECK(NaniteHiZPyramidMipCount(256u, 128u) == 8u);  // 256 → … → 2，正好 8 层
    CHECK(NaniteHiZPyramidMipCount(1920u, 1080u) == kNaniteMaxHiZMips);  // 10 层被钳到 8
    CHECK(NaniteHiZPyramidMipCount(4096u, 4096u) == kNaniteMaxHiZMips);
    CHECK(kNaniteHiZMinMip == 1u);   // mip0 从不被写入 ⇒ 采样下限必须是 1
    MESSAGE("Hi-Z 层数：2x2=" << NaniteHiZPyramidMipCount(2u, 2u)
            << " 4x4=" << NaniteHiZPyramidMipCount(4u, 4u)
            << " 8x8=" << NaniteHiZPyramidMipCount(8u, 8u)
            << " 1920x1080=" << NaniteHiZPyramidMipCount(1920u, 1080u));
}

// ── 25b. 投影：世界球 → 屏幕 AABB + 最近深度（与 shader 的 hizOccluded 前半同式）──
TEST_CASE("NaniteHiZ: 球投影到屏幕 AABB 与最近深度") {
    float m[16];
    MakeTestPerspective(60.0f, 16.0f / 9.0f, 0.1f, 2000.0f, m);
    float rows[16];
    MakeViewProjRows(m, rows);

    // 半径 0 的退化球：盒退化成点，且正好落在屏幕中心（相机朝 -Z、点在轴上）
    {
        const float center[3] = { 0.0f, 0.0f, -10.0f };
        float minUV[2], maxUV[2], nearest = 0.0f;
        REQUIRE(NaniteProjectSphereToScreen(rows, center, 0.0f, minUV, maxUV, &nearest));
        CHECK(std::fabs(minUV[0] - 0.5f) < 1.0e-5f);
        CHECK(std::fabs(minUV[1] - 0.5f) < 1.0e-5f);
        CHECK(std::fabs(maxUV[0] - minUV[0]) < 1.0e-6f);
        // ndc.z(d) = far/(far-near) × (1 - near/d) ⇒ d = 10 时 ≈ 0.990
        CHECK(nearest > 0.98f);
        CHECK(nearest < 1.0f);
    }
    // 半径 1 的球：盒以中心为中心、有正的尺寸；最近深度 < 中心处深度
    {
        const float center[3] = { 0.0f, 0.0f, -10.0f };
        float minUV[2], maxUV[2], nearestNear = 0.0f;
        float minUV0[2], maxUV0[2], nearestFar = 0.0f;
        REQUIRE(NaniteProjectSphereToScreen(rows, center, 1.0f, minUV, maxUV, &nearestNear));
        REQUIRE(NaniteProjectSphereToScreen(rows, center, 0.0f, minUV0, maxUV0, &nearestFar));
        CHECK(minUV[0] < minUV0[0]);
        CHECK(maxUV[0] > maxUV0[0]);
        CHECK(nearestNear < nearestFar);
        // 10 单位处半径 1 ⇒ 屏幕半宽 ≈ 1/10 × focal / (0.5×height)；用 focal 反算的一致性：
        //   tan(fov/2) = 0.5×H/focal，此处 focal = 0.5/sin(30°)；只断言"尺寸 > 0 且 < 半屏"
        CHECK((maxUV[0] - minUV[0]) > 0.0f);
        CHECK((maxUV[0] - minUV[0]) < 1.0f);
    }
    // 相机之后（点在 +Z 侧）⇒ 投影无意义 ⇒ 必须返回 false（调用方按"保守不剔除"处理）
    {
        const float behind[3] = { 0.0f, 0.0f, 10.0f };
        float minUV[2], maxUV[2], nearest = 0.0f;
        CHECK_FALSE(NaniteProjectSphereToScreen(rows, behind, 1.0f, minUV, maxUV, &nearest));
    }
    // 空指针防御
    {
        const float center[3] = { 0.0f, 0.0f, -10.0f };
        float minUV[2], maxUV[2], nearest = 0.0f;
        CHECK_FALSE(NaniteProjectSphereToScreen(nullptr, center, 1.0f, minUV, maxUV, &nearest));
        CHECK_FALSE(NaniteProjectSphereToScreen(rows, nullptr, 1.0f, minUV, maxUV, &nearest));
        CHECK_FALSE(NaniteProjectSphereToScreen(rows, center, 1.0f, nullptr, maxUV, &nearest));
        CHECK_FALSE(NaniteProjectSphereToScreen(rows, center, 1.0f, minUV, maxUV, nullptr));
    }
}

// ── 25c. 选层：ceil(log2(最长边)) 且下限钳到 1、上限钳到 mipCount-1 ──
TEST_CASE("NaniteHiZ: 选层公式与边界") {
    CHECK(NaniteHiZSelectMip(0.0f, 0.0f, 8u) == 1u);
    CHECK(NaniteHiZSelectMip(1.0f, 1.0f, 8u) == 1u);
    CHECK(NaniteHiZSelectMip(2.0f, 1.0f, 8u) == 1u);
    CHECK(NaniteHiZSelectMip(3.0f, 1.0f, 8u) == 2u);
    CHECK(NaniteHiZSelectMip(4.0f, 4.0f, 8u) == 2u);
    CHECK(NaniteHiZSelectMip(32.0f, 8.0f, 8u) == 5u);
    CHECK(NaniteHiZSelectMip(64.0f, 64.0f, 8u) == 6u);
    CHECK(NaniteHiZSelectMip(200.0f, 1.0f, 8u) == 7u);   // 上限 = mipCount-1 = 7
    CHECK(NaniteHiZSelectMip(1000.0f, 1000.0f, 4u) == 3u);  // 层数少时上限跟着降
    CHECK(NaniteHiZSelectMip(1000.0f, 1000.0f, 1u) == kNaniteHiZMinMip);  // 没有可采样层
    CHECK(NaniteHiZSelectMip(1000.0f, 1000.0f, 0u) == kNaniteHiZMinMip);
    // NaN 尺寸 ⇒ 退化为 1（不产生 NaN 层号）；负尺寸同理
    const float nanValue = std::nanf("");
    CHECK(NaniteHiZSelectMip(nanValue, 1.0f, 8u) == 1u);
    CHECK(NaniteHiZSelectMip(-5.0f, -5.0f, 8u) == 1u);
}

// ── 25d. Hi-Z 遮挡判据的两档（开关关闭 = 恒不遮挡；打开时按最近深度 > 采样值判遮挡）──
TEST_CASE("NaniteHiZ: 遮挡判据与开关两档") {
    float m[16];
    MakeTestPerspective(60.0f, 16.0f / 9.0f, 0.1f, 2000.0f, m);
    float rows[16];
    MakeViewProjRows(m, rows);

    SyntheticHiZ hiz;
    for (u32 i = 0u; i < kNaniteMaxHiZMips; ++i) hiz.depthPerMip[i] = 0.995f;  // 屏幕上几乎处处有"很近的遮挡物"
    const NaniteHiZSampler sampler{ &SyntheticHiZSample, &hiz };

    const float near10[3]  = { 0.0f, 0.0f, -10.0f };
    const float near500[3] = { 0.0f, 0.0f, -500.0f };   // ndc.z ≈ 0.9998 > 0.995 ⇒ 应被遮挡
    const float farOff[3]  = { 10000.0f, 0.0f, -10.0f };  // 完全在屏幕外 ⇒ 保守不剔除

    // 【关闭档】mipCount < 2（含 0）⇒ 恒不遮挡，且**一次都不采样**（不浪费带宽）
    hiz.sampleCalls = 0u;
    CHECK_FALSE(NaniteHiZOccluded(rows, near500, 1.0f, 1920.0f, 1080.0f, 0u, sampler));
    CHECK_FALSE(NaniteHiZOccluded(rows, near500, 1.0f, 1920.0f, 1080.0f, 1u, sampler));
    CHECK(hiz.sampleCalls == 0u);
    // 空采样器同样是"关闭"（生产路径的 CPU 参考就是这一档）
    CHECK_FALSE(NaniteHiZOccluded(rows, near500, 1.0f, 1920.0f, 1080.0f, 8u, NaniteHiZSampler{}));

    // 【打开档】
    CHECK_FALSE(NaniteHiZOccluded(rows, near10, 1.0f, 1920.0f, 1080.0f, 8u, sampler));   // 0.99 < 0.99x ⇒ 在前
    CHECK(NaniteHiZOccluded(rows, near500, 1.0f, 1920.0f, 1080.0f, 8u, sampler));        // 0.9998 > 0.99 ⇒ 被挡
    CHECK(hiz.sampleCalls >= 4u);       // 四个角各采一次
    CHECK(hiz.lastMip >= 1u);           // 采样层下限是 1（mip0 从不被写入）

    // 屏幕尺寸为 0 ⇒ 关掉（避免除零/无意义的层计算）
    CHECK_FALSE(NaniteHiZOccluded(rows, near500, 1.0f, 0.0f, 1080.0f, 8u, sampler));
    CHECK_FALSE(NaniteHiZOccluded(rows, near500, 1.0f, 1920.0f, 0.0f, 8u, sampler));
    // 完全在屏幕外的球（沿 X 偏出视锥）⇒ 保守不剔除（Hi-Z 只覆盖已光栅化区域）
    CHECK_FALSE(NaniteHiZOccluded(rows, farOff, 0.5f, 1920.0f, 1080.0f, 8u, sampler));
    // 【可解释性】遮挡只与"采样值 < 最近深度"有关：把合成金字塔改成 1.0（全屏都在最远处）⇒ 一个都不遮挡
    for (u32 i = 0u; i < kNaniteMaxHiZMips; ++i) hiz.depthPerMip[i] = 1.0f;
    CHECK_FALSE(NaniteHiZOccluded(rows, near500, 1.0f, 1920.0f, 1080.0f, 8u, sampler));
    MESSAGE("Hi-Z 两档：关闭恒不遮挡（0 次采样）；打开且金字塔=0.995 时 500 单位的球被遮挡、"
            "10 单位的球不遮挡；金字塔=1.0 时都不遮挡");
}

// ── 25e. LOD 选择：距离越远级别越粗（单调性）+ 恰好选中一级 ──
TEST_CASE("NaniteLOD: 选择级别的单调性与唯一性") {
    const float stepErrors[5] = { 0.01f, 0.1f, 1.0f, 10.0f, 100.0f };
    std::vector<NaniteClusterLODInfo> chain;
    MakeLODChain(stepErrors, 5u, chain);
    REQUIRE(chain.size() == 6u);

    const float focal = 935.0f;            // ≈ 1080p / 60° 的像素焦距（0.5×1080/tan30°）
    const float threshold = kNaniteLODThresholdPixels;

    u32 previous = 0u;
    bool first = true;
    const float distances[9] = { 0.1f, 1.0f, 10.0f, 100.0f, 1000.0f, 5000.0f, 50000.0f, 5.0e5f, 5.0e6f };
    for (float distance : distances) {
        const u32 level = SelectedLevelOnChain(chain, distance, focal, threshold);
        REQUIRE(level != 0xFFFFFFFFu);                 // 每条链上必须恰好选中一级
        if (!first) CHECK(level >= previous);          // 距离越远 ⇒ 级别越粗（单调不减）
        previous = level;
        first = false;
        // 唯一性：把选中的那一级排除后，剩下的级一个都不该被选
        u32 selectedCount = 0u;
        for (const NaniteClusterLODInfo& info : chain) {
            if (NaniteLODClusterSelected(info, distance, focal, threshold)) ++selectedCount;
        }
        CHECK(selectedCount == 1u);
    }
    // 端点：贴脸 ⇒ 最细（级 0）；极远 ⇒ 根（级 5）
    CHECK(SelectedLevelOnChain(chain, 0.1f, focal, threshold) == 0u);
    CHECK(SelectedLevelOnChain(chain, 5.0e6f, focal, threshold) == 5u);
    // 极远时根被选，是因为"没有父簇可比"（根标志），不是因为它自带 parentError = 0
    CHECK(chain.back().parentError == 0.0f);
    CHECK((chain.back().flags & kNaniteLODInfoFlagRoot) != 0u);
    // 【关闭档】focalPixels <= 0 ⇒ 不筛任何簇（所有级都"被选"）
    for (const NaniteClusterLODInfo& info : chain) {
        CHECK(NaniteLODClusterSelected(info, 10.0f, 0.0f, threshold));
        CHECK(NaniteLODClusterSelected(info, 10.0f, -1.0f, threshold));
    }
    // 距离为 0 / NaN ⇒ 按下限兜底（不产生 inf/NaN 判定）
    CHECK(NaniteLODClusterSelected(chain[0], 0.0f, focal, threshold));
    CHECK_FALSE(NaniteLODClusterSelected(chain[5], 0.0f, focal, threshold));   // 根的 ownError 很大 ⇒ 需要更细
    const float nanValue = std::nanf("");
    CHECK(NaniteLODClusterSelected(chain[0], nanValue, focal, threshold));
    // 阈值越大 ⇒ 越容易"够好"（选更粗的级）
    CHECK(SelectedLevelOnChain(chain, 100.0f, focal, 1000.0f) >=
          SelectedLevelOnChain(chain, 100.0f, focal, 1.0f));
    MESSAGE("LOD 单调性：距离 0.1→5e6 时选中级别 = "
            << SelectedLevelOnChain(chain, 0.1f, focal, threshold) << "→"
            << SelectedLevelOnChain(chain, 10.0f, focal, threshold) << "→"
            << SelectedLevelOnChain(chain, 1000.0f, focal, threshold) << "→"
            << SelectedLevelOnChain(chain, 5.0e6f, focal, threshold));
}

// ── 25f. 三阶段 CPU 参考遍历：已知进/出/遮挡 + 级分布 + 复现性 + 边界 ──
TEST_CASE("NaniteCull3: 三阶段参考遍历的已知进/出/遮挡用例") {
    // 手搭 3 节点树：根 → 左叶（簇 0、1）、右叶（簇 2）。
    // 【坐标口径】相机在原点、朝 -Z（`MakeTestPerspective` 的 view = I）⇒ 簇必须放在 **-Z** 侧，
    //   否则它们落在相机平面上（`clip.w = 0`）⇒ Hi-Z 投影按"保守不剔除"提前返回，用例就测不到遮挡。
    NaniteBVHNode nodes[3];
    nodes[0] = MakeInnerNode(4.5f, 0.0f, -10.0f, 40.0f, 1u, 2u);
    nodes[1] = MakeLeafNode(2.5f, 0.0f, -10.0f, 20.0f, 0u, 2u);
    nodes[2] = MakeLeafNode(9.0f, 0.0f, -10.0f, 20.0f, 2u, 1u);
    const u32 leaves[3] = { 0u, 1u, 2u };
    NaniteClusterSphere spheres[3] = {};
    spheres[0].center[0] = 0.0f;  spheres[0].center[2] = -10.0f;  spheres[0].radius = 2.0f;
    spheres[1].center[0] = 5.0f;  spheres[1].center[2] = -10.0f;  spheres[1].radius = 2.0f;
    spheres[2].center[0] = 9.0f;  spheres[2].center[2] = -10.0f;  spheres[2].radius = 2.0f;

    NaniteClusterBVHView view;
    view.nodes              = nodes;
    view.nodeCount          = 3u;
    view.leafClusterIndices = leaves;
    view.clusterSpheres     = spheres;
    view.clusterCount       = 3u;

    const NaniteInstanceGpuObject instance = MakeBVHTranslatedInstance(0.0f, 0.0f, 0.0f, 36u);
    const NaniteFrustumPlanes frustum = MakeBVHBoxFrustum(40.0f);   // 三个簇全在盒内

    // 真实的透视 vpRows（Hi-Z 投影用；与盒状视锥各管一段判据，互不干扰）
    float m[16];
    MakeTestPerspective(60.0f, 16.0f / 9.0f, 0.1f, 2000.0f, m);
    float rows[16];
    MakeViewProjRows(m, rows);

    // 【已知用例 1：Phase 1 掩码 = 0 ⇒ 整个实例都不遍历】
    {
        const u32 maskOff[1] = { 0u };
        NaniteCullChainDesc chain;
        chain.visibleMask = maskOff;
        std::vector<NaniteVisibleClusterRef> out(64u);
        NaniteClusterBVHTraversalStats stats{};
        const u32 written = NaniteTraverseClusterBVHCPU(frustum, view, &instance, 1u, 4u,
                                                       out.data(), (u32)out.size(), &stats, chain);
        CHECK(written == 0u);
        CHECK(stats.traversedInstances == 0u);
        CHECK(stats.frustumPassClusters == 0u);
    }

    // 【已知用例 2：掩码 = 1、Hi-Z 关闭、LOD 关闭 ⇒ 三个簇全可见，phase2 = phase3 = 3】
    {
        const u32 maskOn[1] = { 1u };
        NaniteCullChainDesc chain;
        chain.visibleMask = maskOn;
        chain.vpRows[0] = 0.0f;   // 故意不填：Hi-Z 关闭时投影不该被用到
        std::vector<NaniteVisibleClusterRef> out(64u);
        NaniteClusterBVHTraversalStats stats{};
        const u32 written = NaniteTraverseClusterBVHCPU(frustum, view, &instance, 1u, 4u,
                                                       out.data(), (u32)out.size(), &stats, chain);
        CHECK(written == 3u);
        CHECK(stats.frustumPassClusters == 3u);
        CHECK(stats.occludedClusters == 0u);
        CHECK(stats.lodRejectedClusters == 0u);
        CHECK(stats.traversedInstances == 1u);
    }

    // 【已知用例 3：Hi-Z 打开且合成金字塔=0（等价"全屏都被挡住"）⇒ 三个簇全被剔除】
    {
        SyntheticHiZ hiz;                       // depthPerMip 全 0
        const u32 maskOn[1] = { 1u };
        NaniteCullChainDesc chain;
        chain.visibleMask = maskOn;
        for (u32 i = 0u; i < 16u; ++i) chain.vpRows[i] = rows[i];
        chain.screenW = 1920.0f;
        chain.screenH = 1080.0f;
        chain.hizMipCount = 8u;
        chain.hiz = NaniteHiZSampler{ &SyntheticHiZSample, &hiz };
        std::vector<NaniteVisibleClusterRef> outHiZ(64u);
        NaniteClusterBVHTraversalStats statsHiZ{};
        const u32 writtenHiZ = NaniteTraverseClusterBVHCPU(frustum, view, &instance, 1u, 4u,
                                                          outHiZ.data(), (u32)outHiZ.size(),
                                                          &statsHiZ, chain);
        CHECK(writtenHiZ == 0u);
        CHECK(statsHiZ.frustumPassClusters == 3u);   // phase2 前半：仍然通过了视锥
        CHECK(statsHiZ.occludedClusters == 3u);      // phase2 后半：全被遮挡
        CHECK(hiz.sampleCalls > 0u);
    }

    // 【已知用例 4：合成金字塔=1（全屏都在最远处）⇒ 一个都不遮挡；两档差集 = "被遮挡的那批"】
    {
        SyntheticHiZ hiz;
        for (u32 i = 0u; i < kNaniteMaxHiZMips; ++i) hiz.depthPerMip[i] = 1.0f;
        const u32 maskOn[1] = { 1u };
        NaniteCullChainDesc chain;
        chain.visibleMask = maskOn;
        for (u32 i = 0u; i < 16u; ++i) chain.vpRows[i] = rows[i];
        chain.screenW = 1920.0f;
        chain.screenH = 1080.0f;
        chain.hizMipCount = 8u;
        chain.hiz = NaniteHiZSampler{ &SyntheticHiZSample, &hiz };
        std::vector<NaniteVisibleClusterRef> out(64u);
        NaniteClusterBVHTraversalStats stats{};
        const u32 written = NaniteTraverseClusterBVHCPU(frustum, view, &instance, 1u, 4u,
                                                       out.data(), (u32)out.size(), &stats, chain);
        CHECK(written == 3u);
        CHECK(stats.occludedClusters == 0u);
    }

    // 【已知用例 5：Phase 3 的 DAG 割：极远 ⇒ 只有根级（级 1）被选，级分布落在 1】
    {
        std::vector<NaniteClusterLODInfo> chainInfo;
        const float stepErrors[1] = { 1000.0f };   // 级 0 → 级 1 的误差很大 ⇒ 极远时选级 1
        MakeLODChain(stepErrors, 1u, chainInfo);
        NaniteClusterLODInfo infos[3];
        infos[0] = chainInfo[0];   // 簇 0、1：级 0（叶子）
        infos[1] = chainInfo[0];
        infos[2] = chainInfo[1];   // 簇 2：级 1（根）

        const u32 maskOn[1] = { 1u };
        NaniteCullChainDesc chain;
        chain.visibleMask = maskOn;
        chain.lodInfo = infos;
        chain.cameraPos[0] = 0.0f; chain.cameraPos[1] = 0.0f; chain.cameraPos[2] = 0.0f;
        chain.focalPixels = 935.0f;
        chain.lodThresholdPixels = kNaniteLODThresholdPixels;
        std::vector<NaniteVisibleClusterRef> out(64u);
        NaniteClusterBVHTraversalStats stats{};
        const u32 written = NaniteTraverseClusterBVHCPU(frustum, view, &instance, 1u, 4u,
                                                       out.data(), (u32)out.size(), &stats, chain);
        // 三个簇都在 0..30 单位处（离相机很近）⇒ 级 0 的 ownError=0 且级 1 的 parentError 很大
        // ⇒ 级 0 被选；级 1 的 ownError=1000 在 30 单位处投影远超 1 像素 ⇒ 被判"需要更细"而拒绝
        //（这条合成数据里没有级 1 的孩子，所以它是"被拒绝"而不是"换个级"—— 正好覆盖拒绝计数）
        CHECK(written == 2u);
        CHECK(stats.lodHistogram[0] == 2u);
        CHECK(stats.lodHistogram[1] == 0u);
        CHECK(stats.lodRejectedClusters == 1u);
        CHECK(stats.frustumPassClusters == 3u);
        CHECK(stats.visibleClusters == written);
    }

    // 【可复现】同输入两次调用，输出逐位一致（GPU 侧唯一的非确定性是槽位顺序）
    {
        const u32 maskOn[1] = { 1u };
        NaniteCullChainDesc chain;
        chain.visibleMask = maskOn;
        std::vector<NaniteVisibleClusterRef> a(64u), b(64u);
        NaniteClusterBVHTraversalStats sa{}, sb{};
        const u32 wa = NaniteTraverseClusterBVHCPU(frustum, view, &instance, 1u, 4u,
                                                  a.data(), (u32)a.size(), &sa, chain);
        const u32 wb = NaniteTraverseClusterBVHCPU(frustum, view, &instance, 1u, 4u,
                                                  b.data(), (u32)b.size(), &sb, chain);
        REQUIRE(wa == wb);
        a.resize(wa);
        b.resize(wb);
        CHECK(std::memcmp(a.data(), b.data(), (usize)wa * sizeof(NaniteVisibleClusterRef)) == 0);
        CHECK(sa.visitedNodes == sb.visitedNodes);
        CHECK(sa.frustumPassClusters == sb.frustumPassClusters);
    }

    // 【边界】空 BVH、空实例表、0 容量、单簇、退化球（半径 0）
    {
        NaniteCullChainDesc chain;
        std::vector<NaniteVisibleClusterRef> out(8u);
        NaniteClusterBVHTraversalStats stats{};
        NaniteClusterBVHView emptyView;
        CHECK(NaniteTraverseClusterBVHCPU(frustum, emptyView, &instance, 1u, 4u,
                                         out.data(), (u32)out.size(), &stats, chain) == 0u);
        CHECK(NaniteTraverseClusterBVHCPU(frustum, view, nullptr, 0u, 4u,
                                         out.data(), (u32)out.size(), &stats, chain) == 0u);
        CHECK(NaniteTraverseClusterBVHCPU(frustum, view, &instance, 0u, 4u,
                                         out.data(), (u32)out.size(), &stats, chain) == 0u);
        CHECK(NaniteTraverseClusterBVHCPU(frustum, view, &instance, 1u, 0u,
                                         out.data(), (u32)out.size(), &stats, chain) == 0u);
        // 【计数与容量无关】输出指针为空 / 容量 0 时仍照常计数（与 GPU 的槽位口径一致）
        CHECK(NaniteTraverseClusterBVHCPU(frustum, view, &instance, 1u, 4u,
                                         nullptr, 0u, &stats, chain) == 3u);
        CHECK(stats.visibleClusters == 3u);
        // 计数与容量无关（容量 0 时写入 0 条但计数照常）
        const u32 writtenZeroCap = NaniteTraverseClusterBVHCPU(frustum, view, &instance, 1u, 4u,
                                                              nullptr, 0u, &stats, chain);
        CHECK(writtenZeroCap == 3u);
        CHECK(stats.visibleClusters == 3u);

        // 退化球：半径 0 的簇仍参与点测试（可见）
        NaniteClusterSphere flat[1] = {};
        flat[0].center[0] = 0.0f;
        flat[0].radius = 0.0f;
        NaniteBVHNode leaf = MakeLeafNode(0.0f, 0.0f, 0.0f, 0.0f, 0u, 1u);
        NaniteClusterBVHView flatView;
        flatView.nodes = &leaf;
        flatView.nodeCount = 1u;
        flatView.leafClusterIndices = leaves;
        flatView.clusterSpheres = flat;
        flatView.clusterCount = 1u;
        const u32 maskOn[1] = { 1u };
        NaniteCullChainDesc flatChain;
        flatChain.visibleMask = maskOn;
        CHECK(NaniteTraverseClusterBVHCPU(frustum, flatView, &instance, 1u, 4u,
                                         out.data(), (u32)out.size(), &stats, flatChain) == 1u);
    }
}

// ── 25g. 三阶段参数与元数据的布局契约（GPU 侧结构体逐字段对应）──
TEST_CASE("NaniteCull3: 参数与元数据的布局契约") {
    CHECK(sizeof(NaniteClusterLODInfo) == 16u);
    CHECK(offsetof(NaniteClusterLODInfo, ownError) == 0u);
    CHECK(offsetof(NaniteClusterLODInfo, parentError) == 4u);
    CHECK(offsetof(NaniteClusterLODInfo, lodLevel) == 8u);
    CHECK(offsetof(NaniteClusterLODInfo, flags) == 12u);
    CHECK(kNaniteLODInfoFlagRoot == 1u);

    // 直方图必须容得下任务 9 的 6 级 LOD 链
    CHECK(kNaniteLODHistogramLevels >= 6u);
    CHECK(kNaniteLODThresholdPixels == 1.0f);   // 设计 §5.1 的阈值（未改）
    CHECK(kNaniteLODMinDistance > 0.0f);
    CHECK(kNaniteMaxHiZMips == 8u);             // 与 GPUCulling::kHiZMips 一致（镜像常量）

    // 读数结构体的新增字段默认全 0（不传 chain 时天然保持 0）
    const NaniteClusterBVHTraversalStats stats{};
    CHECK(stats.frustumPassClusters == 0u);
    CHECK(stats.occludedClusters == 0u);
    CHECK(stats.lodRejectedClusters == 0u);
    for (u32 i = 0u; i < kNaniteLODHistogramLevels; ++i) CHECK(stats.lodHistogram[i] == 0u);

    // 可选输入结构体的默认值 = 任务 14 的原口径（不过滤实例、不做 Hi-Z、不做 LOD）
    const NaniteCullChainDesc chain{};
    CHECK(chain.visibleMask == nullptr);
    CHECK(chain.lodInfo == nullptr);
    CHECK(chain.focalPixels == 0.0f);
    CHECK(chain.hizMipCount == 0u);
    CHECK_FALSE(chain.lodEnabled());
    CHECK(chain.lodThresholdPixels == kNaniteLODThresholdPixels);
    CHECK(std::is_trivially_copyable<NaniteClusterLODInfo>::value);
    CHECK(std::is_trivially_copyable<NaniteCullChainDesc>::value);
}

// ============================================================
// 26. 任务 16：可见簇 → 间接绘制参数（打包 / 截断 / 零可见簇 / 可复现 / 布局）
//
// 【本节验证什么】任务 16 的缺口是"可见簇列表没有消费者"。这里逐条钉住那段映射：
//   · 字段映射：`indexCount = triangleCount × 3`、`firstIndex = triangleOffset × 3`、
//     `vertexOffset` 原样搬运、`instanceCount = 1`、`firstInstance = 簇号`；
//   · 容量截断：容量不满时只写容量内、**计数照常**（与 GPU 的原子计数同义），并给出截断条数；
//   · 零可见簇：输入为空 ⇒ 写 0 条（绘制端据此画 0 次，不崩、不残留）；
//   · 可复现：同一输入两次打包逐位相同（GPU 侧的槽位顺序不定，故比较口径是"按簇号索引后
//     逐字段比"，本用例直接比字节是因为 CPU 参考的顺序确定）；
//   · 与 CPU 参考遍历串起来：遍历给出的可见簇集合 → 打包命令 → 每条命令都能与簇记录对上。
// ============================================================

namespace {

/// 造一条簇记录：只需 `triangleOffset/triangleCount/vertexOffset` 三个字段参与打包
NaniteClusterRecord MakeDrawRangeCluster(u32 triangleOffset, u32 triangleCount, u32 vertexOffset) {
    NaniteClusterRecord record{};
    record.triangleOffset = triangleOffset;
    record.triangleCount  = triangleCount;
    record.vertexOffset   = vertexOffset;
    return record;
}

} // namespace

// ── 26a. 字段映射（含首/末簇与簇内索引范围）──
TEST_CASE("NaniteWiring: 每簇间接绘制参数的字段映射（首/末簇、簇内索引范围、firstInstance=簇号）") {
    // 三个簇：首簇（第 0 条三角形、1 个三角形）、中间簇、末簇（64 个三角形 = 每簇上限）
    const NaniteClusterRecord clusters[3] = {
        MakeDrawRangeCluster(0u, 1u, 0u),        // 首簇
        MakeDrawRangeCluster(7u, 4u, 33u),       // 中间簇：三角形 [7, 11)、顶点从 33 起
        MakeDrawRangeCluster(1000u, 64u, 541404u),  // 末簇：吃满每簇上限
    };

    // ① 绘制参数：`firstIndex = triangleOffset × 3`、`indexCount = triangleCount × 3`
    const NaniteClusterDrawRange r0 = NaniteMakeClusterDrawRange(clusters[0]);
    CHECK(r0.firstIndex == 0u);
    CHECK(r0.indexCount == 3u);
    CHECK(r0.vertexOffset == 0);
    CHECK(r0._pad == 0u);

    const NaniteClusterDrawRange r1 = NaniteMakeClusterDrawRange(clusters[1]);
    CHECK(r1.firstIndex == 21u);          // 7 × 3
    CHECK(r1.indexCount == 12u);          // 4 × 3
    CHECK(r1.vertexOffset == 33);

    const NaniteClusterDrawRange r2 = NaniteMakeClusterDrawRange(clusters[2]);
    CHECK(r2.firstIndex == 3000u);        // 1000 × 3
    CHECK(r2.indexCount == 192u);         // 64 × 3 = 每簇上限
    CHECK(r2.vertexOffset == 541404);

    // ② 间接命令：五个字段逐项对照（含 firstInstance = 簇号约定）
    const NaniteIndirectCommand c0 = NaniteMakeIndirectCommand(r0, 0u);
    CHECK(c0.indexCount == 3u);
    CHECK(c0.instanceCount == 1u);
    CHECK(c0.firstIndex == 0u);
    CHECK(c0.vertexOffset == 0);
    CHECK(c0.firstInstance == 0u);        // 首簇的簇号 = 0

    const NaniteIndirectCommand c1 = NaniteMakeIndirectCommand(r1, 1u);
    CHECK(c1.indexCount == 12u);
    CHECK(c1.instanceCount == 1u);
    CHECK(c1.firstIndex == 21u);
    CHECK(c1.vertexOffset == 33);
    CHECK(c1.firstInstance == 1u);        // = 簇号，而不是槽位以外的任何东西

    const NaniteIndirectCommand c2 = NaniteMakeIndirectCommand(r2, 2u);
    CHECK(c2.indexCount == 192u);
    CHECK(c2.firstIndex == 3000u);
    CHECK(c2.vertexOffset == 541404);
    CHECK(c2.firstInstance == 2u);        // 末簇

    // ③ 合法性判据：三条都合法；哨兵（未写过的槽位）必然非法
    CHECK(NaniteIsIndirectCommandLegal(c0, 3u));
    CHECK(NaniteIsIndirectCommandLegal(c1, 3u));
    CHECK(NaniteIsIndirectCommandLegal(c2, 3u));
    const NaniteIndirectCommand sentinel = [] {
        NaniteIndirectCommand cmd;
        std::memset(&cmd, 0xFF, sizeof(cmd));   // 与 `NaniteCull` 的哨兵填充同值
        return cmd;
    }();
    CHECK_FALSE(NaniteIsIndirectCommandLegal(sentinel, 3u));

    // ④ 反例逐条：instanceCount ≠ 1 / indexCount 非 3 的倍数 / 超每簇上限 / 簇号越界
    NaniteIndirectCommand bad = c1;
    bad.instanceCount = 2u;
    CHECK_FALSE(NaniteIsIndirectCommandLegal(bad, 3u));
    bad = c1;
    bad.indexCount = 4u;                    // 不是 3 的倍数（画不出整数个三角形）
    CHECK_FALSE(NaniteIsIndirectCommandLegal(bad, 3u));
    bad = c1;
    bad.indexCount = 195u;                  // 65 个三角形 > 每簇上限 64
    CHECK_FALSE(NaniteIsIndirectCommandLegal(bad, 3u));
    bad = c1;
    bad.firstInstance = 3u;                 // 簇号 == clusterCount ⇒ 越界
    CHECK_FALSE(NaniteIsIndirectCommandLegal(bad, 3u));
    CHECK_FALSE(NaniteIsIndirectCommandLegal(c1, 1u));   // 簇号 1 ≥ clusterCount 1

    // ⑤ 逐字段比对函数：一致为真，改任意一个字段即为假
    CHECK(NaniteIndirectCommandMatchesRange(c1, r1, 1u));
    NaniteIndirectCommand tweaked = c1;
    tweaked.firstIndex += 3u;
    CHECK_FALSE(NaniteIndirectCommandMatchesRange(tweaked, r1, 1u));
    tweaked = c1;
    tweaked.vertexOffset += 1;
    CHECK_FALSE(NaniteIndirectCommandMatchesRange(tweaked, r1, 1u));
    CHECK_FALSE(NaniteIndirectCommandMatchesRange(c1, r1, 2u));   // 簇号不一致
}

// ── 26b. 容量截断 / 零可见簇 / 越界与空指针防御 ──
TEST_CASE("NaniteWiring: 可见簇 → 间接命令的容量截断、零可见簇与空指针防御") {
    const NaniteClusterRecord clusters[4] = {
        MakeDrawRangeCluster(0u, 2u, 0u),
        MakeDrawRangeCluster(2u, 3u, 8u),
        MakeDrawRangeCluster(5u, 1u, 16u),
        MakeDrawRangeCluster(6u, 6u, 24u),
    };
    NaniteClusterDrawRange ranges[4] = {};
    for (u32 i = 0u; i < 4u; ++i) ranges[i] = NaniteMakeClusterDrawRange(clusters[i]);

    // 可见引用（顺序任意；这里故意不按簇号升序，证明打包只依赖 refs 的内容）
    const NaniteVisibleClusterRef refs[4] = {
        { 0u, 2u }, { 1u, 0u }, { 0u, 3u }, { 1u, 1u },
    };

    // ① 容量足够：4 条全写，顺序与 refs 一致，字段由簇记录决定（同一个簇在两个实例下字段相同）
    {
        NaniteIndirectCommand out[4] = {};
        u32 truncated = 99u;
        const u32 written = NanitePackVisibleIndirectCommands(refs, 4u, ranges, 4u, out, 4u, &truncated);
        CHECK(written == 4u);
        CHECK(truncated == 0u);
        CHECK(out[0].firstInstance == 2u);
        CHECK(out[1].firstInstance == 0u);
        CHECK(out[2].firstInstance == 3u);
        CHECK(out[3].firstInstance == 1u);
        CHECK(out[0].indexCount == 3u);       // 簇 2：1 个三角形
        CHECK(out[0].firstIndex == 15u);      // 5 × 3
        CHECK(out[1].indexCount == 6u);       // 簇 0：2 个三角形
        CHECK(out[3].vertexOffset == 8);      // 簇 1 的 vertexOffset
    }

    // ② 容量截断：只写容量内 2 条，**计数照常**（截断条数 = 剩下的 2 条），不越界写
    {
        NaniteIndirectCommand out[2] = {};
        u32 truncated = 0u;
        const u32 written = NanitePackVisibleIndirectCommands(refs, 4u, ranges, 4u, out, 2u, &truncated);
        CHECK(written == 2u);
        CHECK(truncated == 2u);               // 4 条可见、只写了 2 条 ⇒ 截断 2 条（GPU 的原子计数不受容量影响）
        CHECK(out[0].firstInstance == 2u);
        CHECK(out[1].firstInstance == 0u);
    }

    // ③ 容量 0 / 空输入 / 零可见簇：一律 0 条命令（绘制端据此画 0 次）
    {
        NaniteIndirectCommand out[1] = {};
        u32 truncated = 7u;
        CHECK(NanitePackVisibleIndirectCommands(refs, 4u, ranges, 4u, out, 0u, &truncated) == 0u);
        CHECK(truncated == 0u);               // 容量 0 ⇒ 提前返回，不统计（调用方也不该用这个读数）
        truncated = 0u;
        CHECK(NanitePackVisibleIndirectCommands(nullptr, 0u, ranges, 4u, out, 1u, &truncated) == 0u);
        CHECK(truncated == 0u);
        truncated = 0u;
        CHECK(NanitePackVisibleIndirectCommands(refs, 0u, ranges, 4u, out, 1u, &truncated) == 0u);
        CHECK(truncated == 0u);               // 零可见簇 ⇒ 零命令、零截断
    }

    // ④ 空指针防御（簇表/引用/输出任一为空 ⇒ 0，且不写越界）
    {
        NaniteIndirectCommand out[1] = {};
        CHECK(NanitePackVisibleIndirectCommands(refs, 4u, nullptr, 4u, out, 1u, nullptr) == 0u);
        CHECK(NanitePackVisibleIndirectCommands(nullptr, 4u, ranges, 4u, out, 1u, nullptr) == 0u);
        CHECK(NanitePackVisibleIndirectCommands(refs, 4u, ranges, 4u, nullptr, 1u, nullptr) == 0u);
    }

    // ⑤ 越界簇下标：该条跳过（不读越界簇表），其余照常写
    {
        const NaniteVisibleClusterRef badRefs[2] = { { 0u, 0u }, { 0u, 99u } };
        NaniteIndirectCommand out[2] = {};
        u32 truncated = 0u;
        const u32 written = NanitePackVisibleIndirectCommands(badRefs, 2u, ranges, 4u, out, 2u, &truncated);
        CHECK(written == 1u);
        CHECK(truncated == 0u);               // 越界是"跳过"，不是"截断"（与 GPU 的 `continue` 同义）
        CHECK(out[0].firstInstance == 0u);
    }
}

// ── 26c. 与 CPU 参考遍历串起来：可见簇集合 → 命令，逐条对得上；两次运行逐位可复现 ──
TEST_CASE("NaniteWiring: CPU 参考遍历 → 命令打包逐条一致且两次运行逐位可复现") {
    // 手搭 1 节点 / 4 簇的最小 BVH（复用任务 14 的用例夹具），簇记录给出真实的索引范围
    const NaniteClusterRecord clusters[4] = {
        MakeDrawRangeCluster(0u, 4u, 0u),
        MakeDrawRangeCluster(4u, 2u, 40u),
        MakeDrawRangeCluster(6u, 8u, 80u),
        MakeDrawRangeCluster(14u, 1u, 120u),
    };
    NaniteClusterDrawRange ranges[4] = {};
    for (u32 i = 0u; i < 4u; ++i) ranges[i] = NaniteMakeClusterDrawRange(clusters[i]);

    const NaniteBVHNode       nodes[1]   = { MakeLeafNode(0.0f, 0.0f, 0.0f, 1.0f, 0u, 4u) };
    const u32                 leaves[4]  = { 0u, 1u, 2u, 3u };
    const NaniteClusterSphere spheres[4] = {
        { { 0.0f, 0.0f, 0.0f }, 1.0f }, { { 1.0f, 0.0f, 0.0f }, 1.0f },
        { { 2.0f, 0.0f, 0.0f }, 1.0f }, { { 3.0f, 0.0f, 0.0f }, 1.0f },
    };
    NaniteClusterBVHView view;
    view.nodes              = nodes;
    view.nodeCount          = 1u;
    view.leafClusterIndices = leaves;
    view.clusterSpheres     = spheres;
    view.clusterCount       = 4u;

    // 两个实例（第二个平移 1.5），都在盒 [-4,4]^3 内 ⇒ 8 条可见引用
    const NaniteInstanceGpuObject instances[2] = {
        MakeBVHTranslatedInstance(0.0f, 0.0f, 0.0f, 36u),
        MakeBVHTranslatedInstance(1.5f, 0.0f, 0.0f, 36u),
    };

    std::vector<NaniteVisibleClusterRef> visible(8u);
    const u32 written = NaniteTraverseClusterBVHCPU(MakeBVHBoxFrustum(4.0f), view, instances, 2u, 4u,
                                                   visible.data(), (u32)visible.size(), nullptr);
    visible.resize(written);
    CHECK(written == 8u);   // 2 实例 × 4 簇

    std::vector<NaniteIndirectCommand> commands(visible.size());
    u32 truncated = 0u;
    const u32 packed = NanitePackVisibleIndirectCommands(
        visible.data(), (u32)visible.size(), ranges, 4u,
        commands.data(), (u32)commands.size(), &truncated);
    CHECK(packed == written);
    CHECK(truncated == 0u);
    commands.resize(packed);

    // ① 每条命令都能与"该簇应有"的参数逐字段对上（首/末簇、簇内索引范围、簇号约定）
    for (u32 i = 0u; i < packed; ++i) {
        const u32 cluster = commands[i].firstInstance;
        REQUIRE(cluster < 4u);
        CHECK(NaniteIsIndirectCommandLegal(commands[i], 4u));
        CHECK(NaniteIndirectCommandMatchesRange(commands[i], ranges[cluster], cluster));
    }
    // ② 首条/末条命令的具体值（可人工核对）
    CHECK(commands.front().firstInstance == visible.front().cluster);
    CHECK(commands.back().firstInstance == visible.back().cluster);
    CHECK(commands.front().indexCount == ranges[visible.front().cluster].indexCount);
    CHECK(commands.back().indexCount == ranges[visible.back().cluster].indexCount);

    // ③ 可复现：同一输入两次打包逐位相同（CPU 侧顺序确定 ⇒ 直接比字节）
    std::vector<NaniteIndirectCommand> again(visible.size());
    u32 truncatedAgain = 0u;
    const u32 packedAgain = NanitePackVisibleIndirectCommands(
        visible.data(), (u32)visible.size(), ranges, 4u,
        again.data(), (u32)again.size(), &truncatedAgain);
    CHECK(packedAgain == packed);
    CHECK(truncatedAgain == 0u);
    again.resize(packedAgain);
    CHECK(std::memcmp(commands.data(), again.data(),
                      sizeof(NaniteIndirectCommand) * packed) == 0);

    // ④ 零可见簇（相机全部背对 ⇒ 撕裂输入为 0）：零命令、零截断
    std::vector<NaniteIndirectCommand> none(1u);
    u32 truncatedZero = 0u;
    CHECK(NanitePackVisibleIndirectCommands(visible.data(), 0u, ranges, 4u,
                                            none.data(), 1u, &truncatedZero) == 0u);
    CHECK(truncatedZero == 0u);

    // 关键读数（人工可核对的一行）：可见簇 8 → 命令 8（截断 0）；首条命令的五个字段
    MESSAGE("可见簇=", written, " → 命令=", packed, " 截断=", truncated,
            "；首条 indexCount=", commands.front().indexCount,
            " firstIndex=", commands.front().firstIndex,
            " vertexOffset=", commands.front().vertexOffset,
            " firstInstance(簇号)=", commands.front().firstInstance,
            "；两次打包逐位一致");
}

// ── 26d. 布局契约与占位索引缓冲的上界（改一个数字就编译失败）──
TEST_CASE("NaniteWiring: 绘制参数/命令的布局契约与占位索引上界") {
    CHECK(sizeof(NaniteClusterDrawRange) == 16u);
    CHECK(offsetof(NaniteClusterDrawRange, firstIndex) == 0u);
    CHECK(offsetof(NaniteClusterDrawRange, indexCount) == 4u);
    CHECK(offsetof(NaniteClusterDrawRange, vertexOffset) == 8u);
    CHECK(offsetof(NaniteClusterDrawRange, _pad) == 12u);
    CHECK(std::is_trivially_copyable<NaniteClusterDrawRange>::value);

    // 命令仍是任务 3 定稿的 20B（= VkDrawIndexedIndirectCommand）
    CHECK(sizeof(NaniteIndirectCommand) == 20u);
    CHECK(offsetof(NaniteIndirectCommand, indexCount) == 0u);
    CHECK(offsetof(NaniteIndirectCommand, instanceCount) == 4u);
    CHECK(offsetof(NaniteIndirectCommand, firstIndex) == 8u);
    CHECK(offsetof(NaniteIndirectCommand, vertexOffset) == 12u);
    CHECK(offsetof(NaniteIndirectCommand, firstInstance) == 16u);

    // 命令缓冲与可见簇引用表**同容量**（槽位一一对应 ⇒ 读回比对不需要映射）
    CHECK(kNaniteMaxIndirectDraws == kNaniteMaxVisibleClusterRefs);
    CHECK(kNaniteMaxIndirectDraws == kNaniteMaxBVHInstances * kNaniteMaxBVHClusters);

    // 占位索引缓冲的上界必须覆盖"最坏簇"的索引范围：
    //   firstIndex + indexCount ≤ 簇数上限 × 每簇三角形上限 × 3（簇是索引段里的连续三角形区间）
    CHECK(kNanitePlaceholderIndexCountMax == 16384u * 64u * 3u);
    const u32 worstFirstIndex   = (kNaniteMaxBVHClusters - 1u) * kNaniteMaxClusterTriangles
                                * kNaniteIndicesPerTriangle;
    const u32 worstIndexCount   = kNaniteMaxClusterTriangles * kNaniteIndicesPerTriangle;
    CHECK(worstFirstIndex + worstIndexCount <= kNanitePlaceholderIndexCountMax);

    // 假簇链的最小容量也必须落在占位缓冲内（退化路径的第一帧：资产还没上传）
    CHECK(kNaniteFakeClusterIndexCount <= kNanitePlaceholderIndexCountMax);
}

// ── 27. 【任务 23】可见簇的簇大小分布五桶：槽位约束 + 区间边界 + 全枚举映射 ──────────────
//
// 【这条测试钉住什么】"五桶之和 == 软光栅分流两侧合计"这条不变式的前提是**分桶是全覆盖且互斥的**，
//   而全覆盖/互斥由两件事决定：① 槽位连续且落在读数缓冲内（C++ 与 Slang 必须同一组数字）；
//   ② 区间边界与 Slang 的 `softRasterSizeBucket()` 逐分支等价。两者都在这里被枚举钉住，
//   于是 shader 侧改错一处（例如把 `<= 16` 写成 `< 16`）会先在单测红掉，而不是等到读数对不上。
TEST_CASE("NaniteSizeDist: 五桶槽位/区间与 1..64 全覆盖映射（任务 23）") {
    // ① 槽位：紧跟既有的 14 号槽、连续；**后面紧接任务 24 的两个流式槽**（20/21），
    //   再往后是任务 26 的平局槽（22）。
    //   【任务 26 起的口径变化】读数缓冲从 22 扩到 23，五桶仍"不许留空洞"，
    //   而"吃满容量"这条口径转由任务 26 的平局槽承担（C++/Slang 两处的 static_assert 钉住）。
    CHECK(kNaniteSoftStatSizeBucket0 == 15u);
    CHECK(kNaniteSoftStatSizeBucket0 == kNaniteSoftStatDepthResolvedPixels + 1u);
    CHECK(kNaniteSoftStatSizeBucketCount == 5u);
    CHECK(kNaniteSoftStatsCapacity == 23u);
    CHECK(kNaniteSoftStatSizeBucket0 + kNaniteSoftStatSizeBucketCount
          == kNaniteSoftStatPageMissClusters);
    CHECK(kNaniteSoftStatPageMissClusters == 20u);
    CHECK(kNaniteSoftStatPageRequests == 21u);
    // 【任务 26 / §14.34 第 7 行】平局槽：紧接流式槽（21 → 22）且吃满容量（容量 23）。
    //   这两个断言就是"槽位连续、吃满容量"这条互锁纪律在单测里的落点：
    //   改动槽位而不改容量（或反之）会先在编译期 static_assert 红掉，再在这里被钉一次。
    CHECK(kNaniteSoftStatDepthKeyTies == 22u);
    CHECK(kNaniteSoftStatPageRequests + 1u == kNaniteSoftStatDepthKeyTies);
    CHECK(kNaniteSoftStatDepthKeyTies + 1u == kNaniteSoftStatsCapacity);

    // ② 区间：闭区间上界表 = 任务书写的 1-4 / 5-8 / 9-16 / 17-32 / 33-64
    CHECK(kNaniteSizeBucketUpperBound[0] == 4u);
    CHECK(kNaniteSizeBucketUpperBound[1] == 8u);
    CHECK(kNaniteSizeBucketUpperBound[2] == 16u);
    CHECK(kNaniteSizeBucketUpperBound[3] == 32u);
    CHECK(kNaniteSizeBucketUpperBound[4] == 64u);
    // 最后一桶的上界必须正好是簇三角形上限：否则 [65, 合法上限] 之间会漏掉合法簇
    CHECK(kNaniteSizeBucketUpperBound[kNaniteSoftStatSizeBucketCount - 1u] == kNaniteMaxClusterTriangles);

    // ③ 全枚举：1..64（合法簇的整个范围）逐个查表，并与**手写的区间表**比对
    //    （手写表 = Slang 里那 5 级阶梯的逐字翻译 ⇒ 这条 CHECK 就是两侧一致性的证据）
    for (u32 tri = 1u; tri <= kNaniteMaxClusterTriangles; ++tri) {
        const u32 expected = tri <= 4u ? 0u : tri <= 8u ? 1u : tri <= 16u ? 2u : tri <= 32u ? 3u : 4u;
        CHECK(NaniteSizeBucketOf(tri) == expected);
    }
    // 边界两侧成对检查（区间是**闭**的：4 与 5 必须落在相邻两个桶，不能同桶）
    CHECK(NaniteSizeBucketOf(1u) == 0u);
    CHECK(NaniteSizeBucketOf(4u) == 0u);
    CHECK(NaniteSizeBucketOf(5u) == 1u);
    CHECK(NaniteSizeBucketOf(8u) == 1u);
    CHECK(NaniteSizeBucketOf(9u) == 2u);
    CHECK(NaniteSizeBucketOf(16u) == 2u);
    CHECK(NaniteSizeBucketOf(17u) == 3u);
    CHECK(NaniteSizeBucketOf(32u) == 3u);
    CHECK(NaniteSizeBucketOf(33u) == 4u);
    CHECK(NaniteSizeBucketOf(64u) == 4u);
    // ④ 越界夹取：> 64（只可能来自损坏资产）归入最后一桶，绝不越界
    CHECK(NaniteSizeBucketOf(65u) == 4u);
    CHECK(NaniteSizeBucketOf(0xFFFFFFFFu) == 4u);

    // ⑤ 穷举"全覆盖且互斥"：对每个合法三角形数，桶号唯一且落在 [0,桶数)
    //    （互斥性是"五桶之和 == 簇数"的必要条件；这里用计数法再验一次：所有合法值恰好各落一个桶）
    u32 perBucket[kNaniteSoftStatSizeBucketCount] = {};
    for (u32 tri = 1u; tri <= kNaniteMaxClusterTriangles; ++tri) {
        const u32 b = NaniteSizeBucketOf(tri);
        REQUIRE(b < kNaniteSoftStatSizeBucketCount);
        ++perBucket[b];
    }
    CHECK(perBucket[0] == 4u);    // 1..4
    CHECK(perBucket[1] == 4u);    // 5..8
    CHECK(perBucket[2] == 8u);    // 9..16
    CHECK(perBucket[3] == 16u);   // 17..32
    CHECK(perBucket[4] == 32u);   // 33..64
    CHECK(perBucket[0] + perBucket[1] + perBucket[2] + perBucket[3] + perBucket[4]
          == kNaniteMaxClusterTriangles);
    MESSAGE("五桶区间 = 1-4(", perBucket[0], ") / 5-8(", perBucket[1], ") / 9-16(", perBucket[2],
            ") / 17-32(", perBucket[3], ") / 33-64(", perBucket[4], ")，合计 = 合法簇三角形上限 ",
            kNaniteMaxClusterTriangles);
}

