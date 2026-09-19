#pragma once

// ============================================================
// Nanite/NaniteTypes.h — Nanite 模块的纯数据类型与 objectIndex 分区契约
//
// 【本文件由 §14.8 任务 1 建立骨架】
//   任务 1 只放"编译得过、可被别处 include"的 POD 与常量；真正的数据格式（文件头/顶点/
//   索引/cone）在任务 7 定稿，量化与打包的边界判据在任务 10/11。
//   任务 5 定稿了 objectIndex 分区契约（分区表 + 边界/换算函数 + 实例槽分配器，见下），
//   `Tests/TestNaniteTypes.cpp` 把边界逐点钉住。
//
// 【为什么必须 RHI-free】§14.7：Scene 侧的 `MeshComponent` 只存"资产路径 + 不透明 u64 句柄"，
//   它需要 include 本头文件取常量与 POD，却**不能**因此牵入 Render 的类型（否则形成
//   Scene → Render 的反向依赖）。因此本文件只允许依赖 `Core/Types.h` 与标准库：
//   **不得 include 任何 RHI 头**，`rhi::` 类型、纹理/缓冲句柄一律不得出现在这里。
//
// 【§14.3 依赖禁令（Nanite 模块内每个文件共同遵守）】
//   · 模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的**内部结构**
//     （可以借其 Hi-Z 纹理句柄与描述符写法，但那属于后续任务在 .cpp 里的实现细节）；
//   · 不得依赖 `MeshBatcher` 的运行时状态 —— 它只当**一次性输入**（合并几何）。
//   · 公共面只有三个：`NaniteRenderer`（生命周期 + 帧图接入）、`NaniteSettings`（开关/档位）、
//     本文件的 POD。
// ============================================================

#include "Core/Types.h"

#include <cstddef>   // offsetof（钉住 POD 的字段偏移）

namespace he::render {

// ============================================================
// 光栅档位（§14.4 面板下拉："软光栅 / 混合光栅"）
//
// 任务 1 只放**枚举占位**：真值落在 `NaniteSettings::rasterMode`，当前没有任何 pass 消费它。
// 软光栅的落点（A1 给 GBuffer 加 UAV / A2 模块自建 VisBuffer）由任务 4 裁决（§14.5）。
// ============================================================
enum class NaniteRasterMode : u32 {
    Soft   = 0,   // 软光栅：compute 写 GBuffer（路线见 §14.5，任务 4 裁决 A1/A2）
    Hybrid = 1,   // 混合光栅：软光栅 + mesh shader 硬光栅分支（任务 6 / 22 起）
};

// ============================================================
// objectIndex 分区契约（§14.5「模块实例占独立 index 空间」；§14.8 任务 5 定稿）
//
// ── 1. objectIndex 是什么、写在哪、谁解码（任务 5 的测量结论）──────────────
//   · 唯一的写入点：`GBuffer.frag.slang:134`
//       output.lightmapKey = float4(origin + saturate(tuv) * tile, float(objectIndex), 0.0);
//     MRT7 = 光照图键，格式 **RGBA16_FLOAT**（`GBufferRenderer.h:26/137`，`kGBufferSlotLightmapKey = 7`）：
//       .xy = 页内坐标（按主导轴的箱式投影：6 个面铺成 3×2 个 tile，已归一化到 [0,1]）
//       .z  = **页号 = objectIndex**  ← 本契约管的量
//       .w  = 0
//     片元拿到的 objectIndex 来自 push constant（`GBufferPushConstant.objectIndex`，
//     `ShaderTypes.slang:349`），由 CPU 的绘制列表（MeshBatcher / GPUScene / SceneRenderer）填。
//   · 解码点（全部）：
//     ① `Tools/gi/lightmap_key_check.py:64/82-92` —— 今天**唯一**真正解析页号的地方：
//        `page = key[..., 2]`，并断言 `0 <= page < KGPU_MAX_OBJECTS`（=1024，见 :37/:89-92）。
//     ② `DeferredLighting.frag.slang:30` —— 声明了 `u_LightmapKey` 绑定（描述符在
//        `LightingPass.cpp:136/282` 写入），但**从未采样**：今天没有任何着色器解码页号。
//     ③ 只搬运内容、不做解码的附件读写点：`DecalPass.cpp:234`（贴花不写 MRT7，见
//        `DecalPass.h:22`）、`GBufferRenderer_CPU.cpp:59`、`GBufferRenderer_GPU.cpp:62`、
//        `DeferredPipeline_FrameGraph.cpp:275(Write)/1242(Read)/1316`。
//   · 越界风险（测量结论：今天没有显存越界路径，但离线判据会被 Nanite 页判 FAIL）：
//     按普通段容量索引 `u_Objects[...]` 的访问共 6 个文件 / 7 处 —— `GBuffer.vert.slang:45`、
//     `GBuffer.frag.slang:54`、`PBR.vert.slang:60/66`、`PBR.frag.slang:270`、
//     `Shadow.vert.slang:33`、`RSM_Generate.vert.slang:32`；缓冲大小是
//     `sizeof(GPUObjectData) * MAX_OBJECTS`（`DeferredPipeline.cpp:81`、`ForwardPipeline.cpp:130`、
//     `GI_RSM.cpp:72`）⇒ objectIndex ≥ 1024 就是越界读。它们的索引**全部**来自 push constant /
//     `SV_InstanceID`，没有一个来自 `gb_lightmapkey` ⇒ 现状安全。
//     （`GPUCull*.comp.slang` 里的 `u_SceneObjects[...]` 是**另一个**缓冲、另一套上限
//     `kGPUMaxGPUObjects = 2048`（`ShaderTypes.slang:210`），不在本契约的管辖范围内。）
//     真正按"普通段容量"解码页号的是离线检查 ①（`page < 1024`）：Nanite 页一旦出现
//     （任务 18 起），它会直接判 FAIL。它的最小修法（留给任务 18，本任务不改该工具）：
//     改成按段分类 —— `page < kNormalObjectIndexCapacity` 走普通段，落在 Nanite 段则用
//     `NaniteLocalIndex()` 取本地下标，两段之外（含哨兵）才 FAIL。
//
// ── 2. 分区表 ──────────────────────────────────────────────────────────────
//     普通段   = [0, 1024)              容量 1024  ← 与既有 `GPUObjectData` 缓冲上限一致
//     Nanite 段 = [1024, 2048)           容量 1024  ← 模块**自持** `NaniteInstance` 缓冲的本地下标空间
//     保留哨兵 = `kInvalidObjectIndex` = 0xFFFFFFFF
//     总容量   = 2048
//
//   · 普通段容量 1024 的依据：`GPUObjectData` 缓冲正好是 `sizeof(GPUObjectData) * MAX_OBJECTS`
//     （`DeferredPipeline.cpp:81`、`ForwardPipeline.cpp:130`、`GI_RSM.cpp:72`），而
//     `MAX_OBJECTS = kGPUMaxObjects`（`Material.h:45` / `ShaderTypes.slang:209`）。
//     这里**硬编码**该数字而**不** include `Material.h`：后者会牵入 RHI 头，破坏本文件的
//     RHI-free 约束（§14.7：Scene 侧要 include 本文件）。改 `kGPUMaxObjects` 时必须同步这里
//     —— `NaniteScene.cpp` 有一条 `static_assert(kNormalObjectIndexCapacity == MAX_OBJECTS)`
//     在编译期兜底，`Tests/TestNaniteTypes.cpp` 再钉一次。
//   · Nanite 段容量 1024 的依据（**任务 5 修正了任务 1 的算术**）：MRT7 是 RGBA16_FLOAT，
//     binary16 只有 1+5+10 位有效位，**精确整数**只在 |n| ≤ 2^11 = 2048 内成立
//     （[1024,2048) 的间距是 1，[2048,4096) 的间距变成 2，再往上 4、8…）。
//     任务 1 预置的 16384 是按 binary32 的 2^24 估的 —— 单位错了：页号 2049 在 GBuffer 里会被
//     量化成 2048，`lightmap_key_check` 的"页号是精确整数"判据必然 FAIL。把总容量收到 2048
//     （恰好等于上面的精确上限）让**整个 ID 空间**天然可精确表示，这正是验收
//     "混排场景下 gb_lightmapkey 解析正确"的静态保证。要突破 2048 必须换 MRT7 格式
//     （RGBA32F，或把页号拆成两个通道），那是 GBuffer 的任务，本契约不能单方面放开。
//   · 哨兵 0xFFFFFFFF 的"不冲突"是三重的：整数上不与任何合法索引相等、段判定为 Invalid、
//     作为 binary16 是 NaN（写进 MRT7 也解析不出整数）。
//   · 为什么既有三处枚举一致性契约（`SceneRenderer.cpp:102-142`、`MeshBatcher.cpp:75-86`、
//     `GPUScene.cpp:66-82`）**不受影响**：它们约定的是**普通段内** objectIndex 的枚举顺序
//     （谁排第几个）。Nanite 实例走**独立编号空间**（模块自持 `NaniteInstance` 缓冲，
//     实例内沿用 208B 的材质部分），既不进这三处的枚举，也不与它们的下标互相换算
//     ⇒ 那三处一个字都不用动。这正是独立编号空间的价值。
//   · **硬约束**：Nanite 段的索引不得按普通段解码。任何 `GPUObjectData[idx]` / `u_Objects[idx]`
//     或 `page < kNormalObjectIndexCapacity` 的判定，都必须先用 `IsNormalObjectIndex(idx)`
//     把 Nanite 段挡在外面；Nanite 段只能经 `NaniteLocalIndex()` 索引模块自持缓冲。
// ============================================================

inline constexpr u32 kNormalObjectIndexBegin    = 0u;
inline constexpr u32 kNormalObjectIndexCapacity = 1024u;   // = kGPUMaxObjects = MAX_OBJECTS
inline constexpr u32 kNaniteObjectIndexBegin    = kNormalObjectIndexBegin + kNormalObjectIndexCapacity;
inline constexpr u32 kNaniteObjectIndexCapacity = 1024u;   // 受 MRT7(RGBA16_FLOAT) 精确整数上限约束，见上
inline constexpr u32 kObjectIndexTotalCapacity  = kNaniteObjectIndexBegin + kNaniteObjectIndexCapacity;

/// 保留哨兵：分配失败 / "无实例"的统一返回值（不是合法索引，也不是合法页号）
inline constexpr u32 kInvalidObjectIndex = 0xFFFFFFFFu;

/// RGBA16_FLOAT（binary16）能精确表示的整数上限（**含**）：2^11 = 2048
inline constexpr u32 kLightmapKeyExactObjectIndexLimit = 2048u;

// ── 分区表的编译期钉子：改任何一个数字都会在编译期炸掉，逼调用方同步 ──
static_assert(kNormalObjectIndexBegin == 0u,
              "普通段必须从 0 开始（SceneRenderer/MeshBatcher/GPUScene 的枚举顺序契约依赖它）");
static_assert(kNormalObjectIndexCapacity == 1024u,
              "普通段容量必须等于 kGPUMaxObjects = 1024（GPUObjectData 缓冲上限）；"
              "改 ShaderTypes.slang/Material.h 时必须同步这里");
static_assert(kNaniteObjectIndexBegin == 1024u,
              "Nanite 段必须紧接普通段（任务 1 预置的边界）");
static_assert(kNormalObjectIndexCapacity <= kNaniteObjectIndexBegin, "两段不得重叠");
static_assert(kObjectIndexTotalCapacity <= kLightmapKeyExactObjectIndexLimit,
              "整个 objectIndex 空间必须能被 RGBA16_FLOAT 精确表示，否则 MRT7 页号会被量化");

// ── 段判定 ────────────────────────────────────────────────────────────────

/// objectIndex 落在哪一段（含"落不进任何段"这一支）
enum class ObjectIndexSegment : u32 {
    Invalid = 0,   // 越界或哨兵
    Normal  = 1,   // 普通段 [0, 1024)
    Nanite  = 2,   // Nanite 段 [1024, 2048)
};

/// 普通段：**按普通段解码前的唯一合法前提**（如 `GPUObjectData[idx]` / 页号 < 1024）
[[nodiscard]] constexpr bool IsNormalObjectIndex(u32 objectIndex) {
    return objectIndex >= kNormalObjectIndexBegin && objectIndex < kNaniteObjectIndexBegin;
}

/// 该 objectIndex 是否落在 Nanite 段（任务 5 的解析入口：先判段，再取本地下标）
[[nodiscard]] constexpr bool IsNaniteObjectIndex(u32 objectIndex) {
    return objectIndex >= kNaniteObjectIndexBegin && objectIndex < kObjectIndexTotalCapacity;
}

/// 越界判据：混排场景里任何一个 objectIndex 都必须落在这两段之内
[[nodiscard]] constexpr bool IsValidObjectIndex(u32 objectIndex) {
    return objectIndex < kObjectIndexTotalCapacity;
}

/// 哨兵判据（与"越界"分开表达：哨兵是**有意义**的失败返回值，不是随手的越界值）
[[nodiscard]] constexpr bool IsInvalidObjectIndex(u32 objectIndex) {
    return objectIndex == kInvalidObjectIndex;
}

/// 该索引能不能作为 MRT7 的页号被**精确**写回（binary16 精确整数上限；合法索引恒为真）
[[nodiscard]] constexpr bool IsLightmapKeyPageExact(u32 objectIndex) {
    return objectIndex <= kLightmapKeyExactObjectIndexLimit;
}

/// 段的分类（Invalid = 越界或哨兵）
[[nodiscard]] constexpr ObjectIndexSegment ClassifyObjectIndex(u32 objectIndex) {
    return IsNormalObjectIndex(objectIndex) ? ObjectIndexSegment::Normal
         : IsNaniteObjectIndex(objectIndex) ? ObjectIndexSegment::Nanite
         : ObjectIndexSegment::Invalid;
}

// ── 局部索引 ↔ 全局 objectIndex 的换算（往返必须无损）────────────────────

/// Nanite 段内的本地实例下标（调用方须先判 IsNaniteObjectIndex）
[[nodiscard]] constexpr u32 NaniteLocalIndex(u32 objectIndex) {
    return objectIndex - kNaniteObjectIndexBegin;
}

/// 普通段的本地下标（普通段从 0 起，故恒等于全局索引；与 NaniteLocalIndex 对称）
[[nodiscard]] constexpr u32 NormalLocalIndex(u32 objectIndex) {
    return objectIndex - kNormalObjectIndexBegin;
}

/// 本地下标 → 全局 objectIndex（段起点由 segment 决定；越界/Invalid 返回哨兵）
[[nodiscard]] constexpr u32 NormalObjectIndexFromLocal(u32 localIndex) {
    return kNormalObjectIndexBegin + localIndex;
}

/// 见 NormalObjectIndexFromLocal
[[nodiscard]] constexpr u32 NaniteObjectIndexFromLocal(u32 localIndex) {
    return kNaniteObjectIndexBegin + localIndex;
}

/// 通用换算：本地下标 → 全局 objectIndex（越界或 segment == Invalid 时返回哨兵）
[[nodiscard]] constexpr u32 ObjectIndexFromLocal(ObjectIndexSegment segment, u32 localIndex) {
    if (segment == ObjectIndexSegment::Normal) {
        return localIndex < kNormalObjectIndexCapacity ? NormalObjectIndexFromLocal(localIndex)
                                                       : kInvalidObjectIndex;
    }
    if (segment == ObjectIndexSegment::Nanite) {
        return localIndex < kNaniteObjectIndexCapacity ? NaniteObjectIndexFromLocal(localIndex)
                                                       : kInvalidObjectIndex;
    }
    return kInvalidObjectIndex;
}

/// 全局 objectIndex → (段, 本地下标)；落不进任何段（含哨兵）返回 false 且不改写出参
[[nodiscard]] constexpr bool TrySplitObjectIndex(u32 objectIndex,
                                                 ObjectIndexSegment& outSegment,
                                                 u32& outLocalIndex) {
    const ObjectIndexSegment segment = ClassifyObjectIndex(objectIndex);
    if (segment == ObjectIndexSegment::Invalid) return false;
    outSegment = segment;
    outLocalIndex = (segment == ObjectIndexSegment::Nanite) ? NaniteLocalIndex(objectIndex)
                                                            : NormalLocalIndex(objectIndex);
    return true;
}

// ============================================================
// Nanite 实例槽分配器（§14.8 任务 5）
//
// 【为什么放在本文件】`NaniteScene.h` 已经 include 了 `RHI/RHI.h`，而任务 5 的单测要求
//   **RHI-free**（`Tests/CMakeLists.txt` 只加 `Engine/Render` 到包含路径、不链接
//   HugEngineRender）。所以分配逻辑必须落在本头文件里；`NaniteScene` 只负责持有它，
//   并在容量耗尽时打印**一次**中文告警（分配器本身不打印，保持宿主无关、可单测）。
//
// 【语义】槽位 = Nanite 段的一个 objectIndex（全局值 = kNaniteObjectIndexBegin + 本地下标）。
//   用 64 位位图记录占用，**总是返回当前最小的空闲槽**（确定性：回收后可立即复用，
//   单测不用猜是哪一号）。容量耗尽返回 kInvalidObjectIndex —— 不静默越界、不抛、不崩。
// ============================================================
class NaniteInstanceSlotAllocator {
public:
    static constexpr u32 kCapacity  = kNaniteObjectIndexCapacity;
    static constexpr u32 kWordBits  = 64u;
    static constexpr u32 kWordCount = (kCapacity + kWordBits - 1u) / kWordBits;

    /// 分配一个 Nanite 段槽位：返回**全局** objectIndex；容量耗尽返回 kInvalidObjectIndex
    constexpr u32 Allocate() {
        for (u32 w = 0; w < kWordCount; ++w) {
            const u64 used = m_Used[w];
            if (used == ~0ull) continue;                  // 该 64 槽全占，看下一字
            const u32 bit   = FirstFreeBit(used);
            const u32 local = w * kWordBits + bit;
            if (local >= kCapacity) break;                // 尾字的富余位：容量已满
            m_Used[w] = used | (1ull << bit);
            ++m_Allocated;
            return NaniteObjectIndexFromLocal(local);
        }
        return kInvalidObjectIndex;                       // 容量耗尽（调用方负责告警）
    }

    /// 回收：只接受 Nanite 段里**当前确实被占用**的索引；普通段/哨兵/重复回收一律 false
    constexpr bool Free(u32 objectIndex) {
        if (!IsNaniteObjectIndex(objectIndex)) return false;
        const u32 local = NaniteLocalIndex(objectIndex);
        const u32 w     = local / kWordBits;
        const u64 bit   = 1ull << (local % kWordBits);
        if ((m_Used[w] & bit) == 0ull) return false;
        m_Used[w] &= ~bit;
        --m_Allocated;
        return true;
    }

    /// 该索引是不是本分配器当前占用的槽（非 Nanite 段恒 false）
    [[nodiscard]] constexpr bool IsAllocated(u32 objectIndex) const {
        if (!IsNaniteObjectIndex(objectIndex)) return false;
        const u32 local = NaniteLocalIndex(objectIndex);
        return (m_Used[local / kWordBits] & (1ull << (local % kWordBits))) != 0ull;
    }

    [[nodiscard]] constexpr u32 AllocatedCount() const { return m_Allocated; }
    [[nodiscard]] constexpr u32 FreeCount()      const { return kCapacity - m_Allocated; }
    [[nodiscard]] static constexpr u32 Capacity() { return kCapacity; }

    /// 清空（`NaniteScene::Shutdown` 用）：所有槽回到空闲
    constexpr void Reset() {
        for (u32 w = 0; w < kWordCount; ++w) m_Used[w] = 0ull;
        m_Allocated = 0u;
    }

private:
    /// 最低的 0 位下标（调用方保证 bits != ~0ull，故返回值 < 64）
    [[nodiscard]] static constexpr u32 FirstFreeBit(u64 bits) {
        u32 n = 0;
        while ((bits & 1ull) != 0ull) { bits >>= 1; ++n; }
        return n;
    }

    u64 m_Used[kWordCount] = {};   ///< 1 = 已占用；本地下标 = 字下标 * 64 + 位下标
    u32 m_Allocated = 0;           ///< 已占用数（单独维护，便于 O(1) 读取）
};

static_assert(NaniteInstanceSlotAllocator::kCapacity == kNaniteObjectIndexCapacity,
              "分配器容量必须覆盖整个 Nanite 段");

// ============================================================
// 任务 3（§14.8）：「计数 → 间接绘制」链的共享 POD
//
// 【为什么放在本文件】它是 Nanite 模块与 Slang 通道之间的**二进制契约**：
//   C++ 侧（NaniteCull/NaniteRaster）与 GPU 侧（Nanite_Cull.comp.slang /
//   Nanite_Raster.frag.slang）必须对同一段内存给出完全一致的解释。契约只由
//   `static_assert` 钉住，**不引入任何 RHI 类型**，因此本文件仍是 RHI-free 的。
//
// 【与任务 7 的关系】任务 7 定稿的是"真实 .nanite 资产"的头部/顶点/索引格式；
//   这里只是任务 3 用假数据验证"计数 → 间接绘制"链所需的最小 POD，不含量化与 cone。
// ============================================================

/// 间接绘制命令：必须与 `VkDrawIndexedIndirectCommand` **二进制兼容**
/// （GLSL/Slang 侧见 `Nanite_Cull.comp.slang` 的 `IndirectCmd`）。
struct alignas(4) NaniteIndirectCommand {
    u32 indexCount    = 0;   // 偏移 0
    u32 instanceCount = 0;   // 偏移 4
    u32 firstIndex    = 0;   // 偏移 8
    i32 vertexOffset  = 0;   // 偏移 12（注意是**有符号**，与 Vulkan 一致）
    u32 firstInstance = 0;   // 偏移 16
};
static_assert(sizeof(NaniteIndirectCommand) == 20,
              "间接命令必须是 20 字节（VkDrawIndexedIndirectCommand / DGC 步长）");
static_assert(offsetof(NaniteIndirectCommand, indexCount)    == 0,  "indexCount 必须在偏移 0");
static_assert(offsetof(NaniteIndirectCommand, instanceCount) == 4,  "instanceCount 必须在偏移 4");
static_assert(offsetof(NaniteIndirectCommand, firstIndex)    == 8,  "firstIndex 必须在偏移 8");
static_assert(offsetof(NaniteIndirectCommand, vertexOffset)  == 12, "vertexOffset 必须在偏移 12");
static_assert(offsetof(NaniteIndirectCommand, firstInstance) == 16, "firstInstance 必须在偏移 16");

/// 假簇条目（任务 3 的输入；与 `Nanite_Cull.comp.slang` 的 `FakeCluster` 一致）
struct alignas(4) NaniteFakeCluster {
    u32 clusterId     = 0;   // 簇编号（假数据 = 顺序编号）
    u32 instanceId    = 0;   // 所属实例（任务 3 固定 0：1 个实例）
    u32 triangleCount = 1;   // 该簇三角形数（假数据固定 1）
    u32 _pad          = 0;   // 填充到 16 B
};
static_assert(sizeof(NaniteFakeCluster) == 16, "假簇条目必须 16 字节（4×u32，便于对齐读回）");

// ── 计数缓冲布局 ──
// 计数缓冲是**单个 u32**：GPU compute 用 InterlockedAdd 累加"实际写入的间接命令条数"，
// 绘制端把这个值直接交给 `DrawIndexedIndirectCount`（由它替代 CPU 决定绘制条数）。
inline constexpr u32 kNaniteCountBufferU32 = 1u;
inline constexpr u32 kNaniteCountBufferSize = sizeof(u32) * kNaniteCountBufferU32;

// ── 假簇链路的容量上限 ──
// 间接命令缓冲 / 计数缓冲 / 光栅化计数缓冲都按它分配；`nanite_fake_clusters` 会被
// 钳制到该上限（超出部分不绘制，而不是越界）。
inline constexpr u32 kNaniteMaxFakeClusters = 1024u;

/// 每条间接命令的 indexCount（假数据：1 个三角形 = 3 个索引）。
/// 读回时用它判定"这个槽位确实被 GPU 写过"（未写过的槽位会被 CPU 预填成哨兵值）。
inline constexpr u32 kNaniteFakeClusterIndexCount = 3u;

// ── 绘制端自建的小目标尺寸 ──
// 【为什么是 1×1】绘制端只用它来"数次数"：1×1 目标 + 1×1 视口 ⇒ 每条间接命令恰好
// 产生 1 个片元 ⇒ 片元里的原子加就等于被光栅化的簇数。它**不是** GBuffer 的任何附件，
// 因此这个 pass 对可见画面零影响（§14.2 不变式 1、§14.8 任务 3 的验收）。
inline constexpr u32 kNaniteRasterTargetSize = 1u;

} // namespace he::render
