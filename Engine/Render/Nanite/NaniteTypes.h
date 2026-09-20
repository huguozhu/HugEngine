#pragma once

// ============================================================
// Nanite/NaniteTypes.h — Nanite 模块的纯数据类型与 objectIndex 分区契约
//
// 【本文件由 §14.8 任务 1 建立骨架】
//   任务 1 只放"编译得过、可被别处 include"的 POD 与常量；真正的数据格式（文件头/顶点/
//   索引/cone）在任务 7 定稿，量化与打包的边界判据在任务 10/11。
//   任务 5 定稿了 objectIndex 分区契约（分区表 + 边界/换算函数 + 实例槽分配器，见下），
//   `Tests/TestNaniteTypes.cpp` 把边界逐点钉住。
//   任务 7 定稿了 `.nanite` 文件格式（设计 §8 的四处不一致已裁决并写回 §8），见**文件末节**
//   "§14.8 任务 7：`.nanite` 文件格式定稿"：文件头 / 簇记录 / 顶点记录（含量化偏置）/
//   索引编码 / cone 轴角字段 / 段表与校验函数。
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

#include <cmath>     // lround / fabs / acos（任务 7：量化与 cone 解码）
#include <cstddef>   // offsetof（钉住 POD 的字段偏移）
#include <cstring>   // memcpy（任务 7：校验函数按值读头部，避免对未对齐缓冲做 reinterpret_cast）
#include <limits>    // numeric_limits（任务 7：段表推导的 32 位宿主兜底）

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

// ── §14.8 任务 6：mesh 通道自建的小目标尺寸 ──
// 【为什么又是 1×1】与上面那条同理：mesh shader 输出一个覆盖整个 NDC 的四边形（2 个三角形），
// 1×1 目标 ⇒ 每个三角形恰好 1 个片元 ⇒ 片元的原子加就等于**被光栅化的 mesh 图元数**，
// "非空"读数的语义因此完全确定（本任务下恒为 2）。
// 【为什么不复用 kNaniteRasterTargetSize 那个目标】任务 3 的目标 `usage` 只有 RenderTarget，
// 而任务 6 要用 `CopyTextureToBuffer` 把它读回 host ⇒ 必须带 `TextureUsage::TransferSrc`；
// 给任务 3 的目标加 usage 会改变那条已经验收过的链路，故另建一张独立小目标（互不干扰）。
inline constexpr u32 kNaniteMeshTestTargetSize = 1u;

// ============================================================
// §14.8 任务 7：`.nanite` 文件格式定稿（2026-09-20，RHI-free）
//
// 【四处不一致的裁决（已逐条写回设计 §8，带"定稿（任务 7，2026-09-20）"标注）】
//   · #8 文件头尺寸：**96B**（不是 128B）。字段累加恰好 96B，且 96 = 6×16 天然 16B 对齐
//     ⇒ 按规则①（§8.3 已写"实现时以 96B 为准"）与规则②（16B 对齐且自包含）同时成立；
//     128B 只是旧 `NanitePack.pack_nanite` docstring 的笔误（它实际写的就是 96B）。
//   · #7 顶点记录尺寸：**16B**（不是 12B）。规则②优先"16 字节对齐且自包含"：量化偏置
//     落在记录内（`NaniteVertex::quantBias`），解码不再依赖外部常量表；12B 版本既不 16B
//     对齐，也没有偏置的落点。
//   · #9 量化偏置：编码端**补上 +512**。§8.4 的解码是 `int(raw & 0x3FF) - 512`（有符号
//     SNORM），而旧 `quantize_vertices` 产出的是无符号 0…511 ⇒ 两边差一个 512 偏置。
//     定稿：单轴编码 `raw = clamp(round((v-bboxMin)/maxExtent*511)) + quantBias`，与解码
//     严格互逆；偏置量本身落进顶点记录的 `quantBias` 字段。
//   · #1 cone 字段：**`coneAxisAngle`**（xyz = 单位轴，w = cos(锥半角)）。`coneData` 语义不明
//     （光有名字写不出解码器）；两者同为 float4 / 16B，规则②③都不偏向谁，取"能唯一确定
//     解码、无需外部约定"的那个（正是规则②"自包含"的意图）。
//   · #6 索引编码：**3×u16 打包进 u32[2] = 8B/三角形**（`NanitePackedTriangle`），不是
//     "1 索引 1 个 u32"（12B/三角形）。理由：两个候选都**不是** 16B 对齐 ⇒ 规则②不裁决，
//     落到规则③"取更省方案"⇒ 8B < 12B（索引带宽 −33%）；且 §8.4 的"每簇 ≤128 顶点"
//     让簇内局部下标只需 7 位，u16 绰绰有余。索引段长度按 16B 向上取整，段起点仍 16B 对齐。
//
// 【文件布局（段偏移**不落盘**，由头部计数 + 固定步长推导，见 `NaniteFileLayout`）】
//   [0]                    NaniteFileHeader        96B
//   [+96]                  NaniteClusterRecord[]   clusterCount  × 64B
//   [..]                   NaniteVertex[]          vertexCount   × 16B
//   [..]                   NanitePackedTriangle[]  (indexCount/3) × 8B
//   [..]                   NaniteMaterialRecord[]  materialCount × 8B
//   [..]                   u32[]                   lodLevelCount × 4B
//   每个段的"起点 16B 对齐、长度向上取整到 16B"。文件尾允许有额外字节（不参与校验）。
//
// 【与 Slang 共享】本节的每个结构体都是 C++ 与（任务 10/12 将建立的）
//   `Engine/Shader/Shaders/Nanite/NaniteTypes.slang` 之间的**二进制契约**：字段顺序/类型/
//   偏移必须逐位一致（std430 / StructuredBuffer 视角）。**改这里的布局必须同步三处**：
//   ① 本文件；② 设计 §8；③ Slang 镜像。下面的 `static_assert` 是这条纪律的编译期钉子。
// ============================================================

// ── 文件级常量（魔数 / 版本 / 各段步长与对齐）──

/// 魔数：恰好 8 字节、**不带 NUL 结尾**（比较必须按 8 字节，不能当 C 字符串用）
inline constexpr char kNaniteFileMagic[8] = { 'N', 'A', 'N', 'I', 'T', 'E', '0', '1' };

/// 文件格式版本（任务 7 第一次定稿，= 1）
inline constexpr u32 kNaniteFileVersion = 1u;

/// 段对齐：每个段的起点都按它对齐，且每段长度向上取整到它
inline constexpr usize kNaniteFileAlignment = 16u;

/// 文件头：字段累加 96B（含 `_reserved[8]` 的 32B）
inline constexpr usize kNaniteFileHeaderBytes = 96u;
/// 头部保留的 u32 个数（写 0；段的偏移由计数推导，不占用保留区）
inline constexpr u32 kNaniteFileHeaderReservedU32 = 8u;
/// `flags` 的 bit0：带 DAG（簇图）
inline constexpr u32 kNaniteFileFlagHasDAG = 1u << 0;

inline constexpr usize kNaniteClusterRecordBytes   = 64u;   ///< 簇记录（与 §8.1 的 GPU 布局同构）
inline constexpr usize kNaniteVertexRecordBytes    = 16u;   ///< 量化顶点（含量化偏置，见下）
inline constexpr usize kNaniteIndexBytesPerTriangle = 8u;   ///< 3×u16 打包进 u32[2]
inline constexpr usize kNaniteMaterialRecordBytes  = 8u;    ///< 材质（bindless 纹理 ID 对）
inline constexpr usize kNaniteLodOffsetBytes       = 4u;    ///< 每个 LOD 一个 u32 偏移
/// 每三角形的索引个数（索引总数必须是它的整数倍）
inline constexpr u32   kNaniteIndicesPerTriangle   = 3u;

// ── `.nanite` 文件头（C++ / Python / Slang 共享；**sizeof == 96B**）──
//
// 【为什么保留区不存段偏移】段的偏移与长度是头部计数的**纯函数**（固定步长），落盘只会
// 制造两份可以互相矛盾的真相。`_reserved[8]` 写 0，留给将来（如流式页表）扩展。
struct alignas(16) NaniteFileHeader {
    char  magic[8]  = { 'N', 'A', 'N', 'I', 'T', 'E', '0', '1' };  // 偏移 0：魔数（无 NUL）
    u32   version   = kNaniteFileVersion;   // 偏移 8：版本（= 1）
    u32   clusterCount = 0;                 // 偏移 12：簇数
    u32   vertexCount  = 0;                 // 偏移 16：量化后顶点数
    u32   indexCount   = 0;                 // 偏移 20：索引**总数**（= 三角形数 × 3）
    u32   materialCount = 0;                // 偏移 24：材质数
    u32   lodLevelCount = 0;                // 偏移 28：LOD 层数
    u32   flags         = 0;                // 偏移 32：bit0 = hasDAG
    float bboxMin[3] = { 0.0f, 0.0f, 0.0f };  // 偏移 36：量化范围下界
    float bboxMax[3] = { 0.0f, 0.0f, 0.0f };  // 偏移 48：量化范围上界
    float maxLODError = 0.0f;               // 偏移 60：最大几何误差
    u32   _reserved[kNaniteFileHeaderReservedU32] = {};  // 偏移 64：保留（写 0）
};

static_assert(sizeof(NaniteFileHeader) == kNaniteFileHeaderBytes,
              ".nanite 文件头必须是 96B（§8.3 定稿：不是 128B）");
static_assert(alignof(NaniteFileHeader) == kNaniteFileAlignment,
              ".nanite 文件头必须 16B 对齐（96 = 6×16）");
static_assert(offsetof(NaniteFileHeader, magic)        == 0,  "magic 必须在偏移 0");
static_assert(offsetof(NaniteFileHeader, version)      == 8,  "version 必须在偏移 8");
static_assert(offsetof(NaniteFileHeader, clusterCount) == 12, "clusterCount 必须在偏移 12");
static_assert(offsetof(NaniteFileHeader, vertexCount)  == 16, "vertexCount 必须在偏移 16");
static_assert(offsetof(NaniteFileHeader, indexCount)   == 20, "indexCount 必须在偏移 20");
static_assert(offsetof(NaniteFileHeader, materialCount) == 24, "materialCount 必须在偏移 24");
static_assert(offsetof(NaniteFileHeader, lodLevelCount) == 28, "lodLevelCount 必须在偏移 28");
static_assert(offsetof(NaniteFileHeader, flags)        == 32, "flags 必须在偏移 32");
static_assert(offsetof(NaniteFileHeader, bboxMin)      == 36, "bboxMin 必须在偏移 36");
static_assert(offsetof(NaniteFileHeader, bboxMax)      == 48, "bboxMax 必须在偏移 48");
static_assert(offsetof(NaniteFileHeader, maxLODError)  == 60, "maxLODError 必须在偏移 60");
static_assert(offsetof(NaniteFileHeader, _reserved)    == 64, "_reserved 必须在偏移 64");

// ── cone 轴角字段（§8.1 裁决 #1：`coneAxisAngle`，替代语义不明的 `coneData`）──
//
/// 法线锥：`axis` = 单位锥轴，`cosHalfAngle` = cos(锥半角)。
/// 【"无锥"哨兵】`cosHalfAngle == kNaniteConeNoCullCos`（= -1，半角 180°）表示该簇恒不可
///   被锥剔除，此时 `axis` 允许为 0 向量（叶子/空簇）。
inline constexpr float kNaniteConeNoCullCos = -1.0f;

/// 单位轴的模长容差（打包器算出的轴允许的数值误差）
inline constexpr float kNaniteConeAxisTolerance = 1.0e-3f;

struct alignas(16) NaniteConeAxisAngle {
    float axis[3]      = { 0.0f, 0.0f, 0.0f };      // 偏移 0：单位锥轴
    float cosHalfAngle = kNaniteConeNoCullCos;      // 偏移 12：cos(锥半角)（-1 = 无锥）
};

static_assert(sizeof(NaniteConeAxisAngle) == 16, "cone 轴角字段必须 16B（float4 的语义化写法）");
static_assert(offsetof(NaniteConeAxisAngle, axis)         == 0,  "cone.axis 必须在偏移 0");
static_assert(offsetof(NaniteConeAxisAngle, cosHalfAngle) == 12, "cone.cosHalfAngle 必须在偏移 12");

/// cone 数据合法性：轴近似单位长且 cos ∈ [-1,1]；"无锥"哨兵（w = -1）允许轴为 0
[[nodiscard]] inline bool IsValidConeAxisAngle(float axisX, float axisY, float axisZ,
                                               float cosHalfAngle) {
    if (!(cosHalfAngle >= -1.0f && cosHalfAngle <= 1.0f)) return false;   // 含 NaN 拒绝
    if (cosHalfAngle == kNaniteConeNoCullCos) return true;                // 无锥哨兵：轴不参与
    const float lengthSquared = axisX * axisX + axisY * axisY + axisZ * axisZ;
    return std::fabs(lengthSquared - 1.0f) <= kNaniteConeAxisTolerance;
}

/// 见三标量重载
[[nodiscard]] inline bool IsValidConeAxisAngle(const NaniteConeAxisAngle& cone) {
    return IsValidConeAxisAngle(cone.axis[0], cone.axis[1], cone.axis[2], cone.cosHalfAngle);
}

/// 锥半角（弧度）：`acos(clamp(cosHalfAngle))`；无锥哨兵给出 π
[[nodiscard]] inline float NaniteConeHalfAngleRadians(float cosHalfAngle) {
    const float clamped = cosHalfAngle < -1.0f ? -1.0f
                        : (cosHalfAngle > 1.0f ? 1.0f : cosHalfAngle);
    return std::acos(clamped);
}

// ── 簇记录（`.nanite` 内为 64B；与 §8.1 的 GPU `NaniteCluster` 二进制同构）──
//
// 【与 GPU 侧的关系】任务 10/12 建 `NaniteCluster` 时必须满足
//   `static_assert(sizeof(NaniteCluster) == sizeof(NaniteClusterRecord))` 且逐字段偏移相同。
/// 簇记录（`clusterCount × 64B`）
struct alignas(16) NaniteClusterRecord {
    float boundsCenterRadius[4] = { 0.0f, 0.0f, 0.0f, 0.0f };  // 偏移 0：xyz=center, w=radius
    NaniteConeAxisAngle cone;                                  // 偏移 16：cone 轴角（裁决 #1）
    u32   triangleOffset = 0;      // 偏移 32：**三角形下标**（× 8B = 索引段字节偏移）
    u32   triangleCount  = 0;      // 偏移 36：三角形数（≤ 64）
    u32   vertexOffset   = 0;      // 偏移 40：顶点缓冲起始下标（× 16B = 顶点段字节偏移）
    u32   materialID     = 0;      // 偏移 44：bindless 材质 ID
    float maxParentLODError = 0.0f;  // 偏移 48：切到父级 LOD 的误差阈值
    u32   childClusterOffset = 0;  // 偏移 52：子节点起始索引（0 = 叶子）
    u32   childCount     = 0;      // 偏移 56：子节点数
    u32   _pad           = 0;      // 偏移 60：对齐填充
};

static_assert(sizeof(NaniteClusterRecord) == kNaniteClusterRecordBytes,
              "簇记录必须 64B（§8.1 与 NanitePack 的 clusterCount × 64B 契约）");
static_assert(alignof(NaniteClusterRecord) == kNaniteFileAlignment, "簇记录必须 16B 对齐");
static_assert(offsetof(NaniteClusterRecord, boundsCenterRadius) == 0,  "boundsCenterRadius 在偏移 0");
static_assert(offsetof(NaniteClusterRecord, cone)               == 16, "cone 在偏移 16");
static_assert(offsetof(NaniteClusterRecord, triangleOffset)     == 32, "triangleOffset 在偏移 32");
static_assert(offsetof(NaniteClusterRecord, triangleCount)      == 36, "triangleCount 在偏移 36");
static_assert(offsetof(NaniteClusterRecord, vertexOffset)       == 40, "vertexOffset 在偏移 40");
static_assert(offsetof(NaniteClusterRecord, materialID)         == 44, "materialID 在偏移 44");
static_assert(offsetof(NaniteClusterRecord, maxParentLODError)  == 48, "maxParentLODError 在偏移 48");
static_assert(offsetof(NaniteClusterRecord, childClusterOffset) == 52, "childClusterOffset 在偏移 52");
static_assert(offsetof(NaniteClusterRecord, childCount)         == 56, "childCount 在偏移 56");
static_assert(offsetof(NaniteClusterRecord, _pad)               == 60, "_pad 在偏移 60");

// ── 量化顶点记录（16B；裁决 #7 与 #9）──
//
/// 10 位字段的位宽/掩码与**有符号量化偏置**（§8.4 的解码是 `raw - 512`）
inline constexpr u32 kNaniteVertexQuantBits = 10u;
inline constexpr u32 kNaniteVertexQuantMask = 0x3FFu;
inline constexpr i32 kNaniteVertexQuantBias = 512;
/// 有符号量化值的范围（`raw - bias`）：raw = 0 ⇒ −512，raw = 1023 ⇒ +511
inline constexpr i32 kNaniteVertexQuantMin = -512;
inline constexpr i32 kNaniteVertexQuantMax = 511;

/// 量化顶点（`vertexCount × 16B`）
///
/// 【为什么有 `quantBias` 而不是 `_pad`】裁决 #7/#9：把量化偏置放进记录内 ⇒ 解码
/// `raw - vertex.quantBias` 不需要任何外部常量表（规则②"自包含"）；代价是每顶点 +4B，
/// 换来的是 16B 对齐 + 编码/解码有唯一落点。
struct alignas(16) NaniteVertex {
    u32 packedPosition = 0;   // 偏移 0：R10G10B10A2_SNORM：x[9:0] y[19:10] z[29:20] w[31:30]=1
    u32 packedNormal   = 0;   // 偏移 4：R10G10B10A2_SNORM（xyz；w 保留 0）
    u32 packedUV       = 0;   // 偏移 8：R16G16_UNORM：u[15:0] v[31:16]
    i32 quantBias      = kNaniteVertexQuantBias;  // 偏移 12：量化偏置（默认 +512）
};

static_assert(sizeof(NaniteVertex) == kNaniteVertexRecordBytes,
              "顶点记录必须 16B（§8.4 裁决 #7：不是 12B）");
static_assert(alignof(NaniteVertex) == kNaniteFileAlignment, "顶点记录必须 16B 对齐");
static_assert(offsetof(NaniteVertex, packedPosition) == 0,  "packedPosition 在偏移 0");
static_assert(offsetof(NaniteVertex, packedNormal)   == 4,  "packedNormal 在偏移 4");
static_assert(offsetof(NaniteVertex, packedUV)       == 8,  "packedUV 在偏移 8");
static_assert(offsetof(NaniteVertex, quantBias)      == 12, "quantBias 在偏移 12");

/// R10G10B10A2 位域打包（x/y/z 各 10 位、w 2 位；超出位宽的位被丢弃）
[[nodiscard]] constexpr u32 NanitePackR10G10B10A2(u32 x, u32 y, u32 z, u32 w) {
    return (x & kNaniteVertexQuantMask)
         | ((y & kNaniteVertexQuantMask) << 10)
         | ((z & kNaniteVertexQuantMask) << 20)
         | ((w & 0x3u) << 30);
}

/// R10G10B10A2 位域解包：`channel` 0/1/2 = x/y/z（10 位），3 = w（2 位）
[[nodiscard]] constexpr u32 NaniteUnpackR10G10B10A2(u32 packed, u32 channel) {
    return (channel == 3u) ? ((packed >> 30) & 0x3u)
                           : ((packed >> (channel * 10u)) & kNaniteVertexQuantMask);
}

/// 位置打包：三轴各 10 位 + w = 1（§8.4 的既定约定）
[[nodiscard]] constexpr u32 NanitePackPosition(u32 rawX, u32 rawY, u32 rawZ) {
    return NanitePackR10G10B10A2(rawX, rawY, rawZ, 1u);
}

/// UV 打包：R16G16_UNORM（u 低 16 位、v 高 16 位）
[[nodiscard]] constexpr u32 NanitePackUV(u32 u16Value, u32 v16Value) {
    return (u16Value & 0xFFFFu) | ((v16Value & 0xFFFFu) << 16);
}

/// 见 NanitePackUV
[[nodiscard]] constexpr u32 NaniteUnpackUVU(u32 packed) { return packed & 0xFFFFu; }
/// 见 NanitePackUV
[[nodiscard]] constexpr u32 NaniteUnpackUVV(u32 packed) { return (packed >> 16) & 0xFFFFu; }

/// 把 `raw` 夹到 10 位合法范围（编码端的越界保护）
[[nodiscard]] constexpr u32 NaniteClampRaw10(i32 raw) {
    if (raw < 0) return 0u;
    if (raw > (i32)kNaniteVertexQuantMask) return kNaniteVertexQuantMask;
    return (u32)raw;
}

/// 单轴位置编码：`raw = clamp(round((v - bboxMin)/maxExtent × 511)) + bias`
/// 【裁决 #9 的落点】旧 `quantize_vertices` 少加了 `+ bias`（产出无符号 0…511），
/// 与解码 `int(raw) - 512` 差一个偏置；此处补上，故与 `NaniteDequantizePositionAxis` 互逆。
/// `maxExtent <= 0`（退化轴）或 NaN ⇒ 返回 `bias`（等价于该轴取 `bboxMin`）。
[[nodiscard]] inline u32 NaniteQuantizePositionAxis(float v, float bboxMin, float maxExtent,
                                                    i32 bias = kNaniteVertexQuantBias) {
    if (!(maxExtent > 0.0f)) return NaniteClampRaw10(bias);
    const float normalized = (v - bboxMin) / maxExtent;                 // [0,1] 表示落在盒内
    const float scaled     = normalized * (float)kNaniteVertexQuantMax; // [0,511]
    if (!(scaled == scaled)) return NaniteClampRaw10(bias);             // NaN 兜底
    const float clamped = scaled < (float)kNaniteVertexQuantMin ? (float)kNaniteVertexQuantMin
                        : (scaled > (float)kNaniteVertexQuantMax ? (float)kNaniteVertexQuantMax
                                                                 : scaled);
    return NaniteClampRaw10((i32)std::lround(clamped) + bias);
}

/// 单轴位置解码：与 `NaniteQuantizePositionAxis` 严格互逆（误差 ≤ maxExtent/1022）
[[nodiscard]] inline float NaniteDequantizePositionAxis(u32 raw, float bboxMin, float maxExtent,
                                                        i32 bias = kNaniteVertexQuantBias) {
    const i32 signedValue = (i32)(raw & kNaniteVertexQuantMask) - bias;   // 有符号 SNORM 量化值
    if (!(maxExtent > 0.0f)) return bboxMin;
    return bboxMin + (float)signedValue / (float)kNaniteVertexQuantMax * maxExtent;
}

// ── 三角形索引编码（8B/三角形）──
//
/// 每簇的硬上限（§4.1 / §8.4：≤64 三角形、≤128 顶点）
inline constexpr u32 kNaniteMaxClusterTriangles = 64u;
inline constexpr u32 kNaniteMaxClusterVertices  = 128u;
/// 单个索引字段的位宽上限（u16）
inline constexpr u32 kNaniteIndexMaxU16 = 0xFFFFu;

/// 打包后的三角形：`lo = i0 | (i1 << 16)`、`hi = i2`（高 16 位保留 0）
///
/// 【索引语义】i0/i1/i2 是**簇内局部**顶点下标（`[0, 127]`，见 `kNaniteMaxClusterVertices`）；
/// 全局顶点下标 = `NaniteClusterRecord::vertexOffset + local`。u16 的宽度对 7 位的实际需求
/// 绰绰有余，这是"3×u16 打包"被选中的前提。
struct alignas(4) NanitePackedTriangle {
    u32 lo = 0;   // 偏移 0：i0（低 16 位）| i1（高 16 位）
    u32 hi = 0;   // 偏移 4：i2（低 16 位）| 保留（高 16 位，写 0）
};

static_assert(sizeof(NanitePackedTriangle) == kNaniteIndexBytesPerTriangle,
              "打包三角形必须 8B（§8.5 裁决 #6：3×u16 进 u32[2]，不是 12B）");
static_assert(offsetof(NanitePackedTriangle, lo) == 0, "lo 必须在偏移 0");
static_assert(offsetof(NanitePackedTriangle, hi) == 4, "hi 必须在偏移 4");

/// 打包一个三角形（超 u16 的高位被丢弃；语义合法性另见 `IsValidClusterLocalVertexIndex`）
[[nodiscard]] constexpr NanitePackedTriangle NanitePackTriangle(u32 i0, u32 i1, u32 i2) {
    NanitePackedTriangle triangle;
    triangle.lo = (i0 & kNaniteIndexMaxU16) | ((i1 & kNaniteIndexMaxU16) << 16);
    triangle.hi = (i2 & kNaniteIndexMaxU16);
    return triangle;
}

/// 见 NanitePackTriangle
[[nodiscard]] constexpr u32 NaniteTriangleIndex0(const NanitePackedTriangle& triangle) {
    return triangle.lo & kNaniteIndexMaxU16;
}
/// 见 NanitePackTriangle
[[nodiscard]] constexpr u32 NaniteTriangleIndex1(const NanitePackedTriangle& triangle) {
    return (triangle.lo >> 16) & kNaniteIndexMaxU16;
}
/// 见 NanitePackTriangle
[[nodiscard]] constexpr u32 NaniteTriangleIndex2(const NanitePackedTriangle& triangle) {
    return triangle.hi & kNaniteIndexMaxU16;
}

/// 簇内局部顶点下标是否落在"每簇 ≤128 顶点"的约束内（合法区间 `[0, 127]`）
[[nodiscard]] constexpr bool IsValidClusterLocalVertexIndex(u32 localIndex) {
    return localIndex < kNaniteMaxClusterVertices;
}

// ── 材质记录（8B；字段语义由任务 10/12 细化，步长已定稿）──
/// 材质记录：bindless 纹理 ID 对（§12 Task 4 的 `<2I>`）
struct alignas(4) NaniteMaterialRecord {
    u32 albedoTexture = 0;   // 偏移 0：albedo 纹理的 bindless ID
    u32 normalTexture = 0;   // 偏移 4：normal 纹理的 bindless ID
};

static_assert(sizeof(NaniteMaterialRecord) == kNaniteMaterialRecordBytes,
              "材质记录必须 8B（§12 Task 4 的 materialCount × 8B）");
static_assert(offsetof(NaniteMaterialRecord, albedoTexture) == 0, "albedoTexture 在偏移 0");
static_assert(offsetof(NaniteMaterialRecord, normalTexture) == 4, "normalTexture 在偏移 4");

// ── 段表与校验（RHI-free、可单测）──

/// 由头部计数**推导**出的段表（不落盘）：偏移 = 前面各段长度之和，长度按 16B 向上取整
struct NaniteFileLayout {
    usize headerOffset = 0,   headerBytes = 0;
    usize clusterOffset = 0,  clusterBytes = 0;
    usize vertexOffset = 0,   vertexBytes = 0;
    usize indexOffset = 0,    indexBytes = 0;     ///< 含 16B 对齐填充
    usize materialOffset = 0, materialBytes = 0;  ///< 含 16B 对齐填充
    usize lodOffset = 0,      lodBytes = 0;       ///< 含 16B 对齐填充
    usize totalBytes = 0;     ///< 合法文件的**最小**长度（尾部允许有额外字节）
    u32   triangleCount = 0;  ///< = indexCount / 3
};

/// `.nanite` 校验/推导的失败原因（`None` 以外都是失败；名字见 `NaniteFileErrorName`）
enum class NaniteFileError : u32 {
    None          = 0,   ///< 合法
    NullData      = 1,   ///< 数据指针为空
    TooSmall      = 2,   ///< 连 96B 头部都读不出来（截断）
    BadMagic      = 3,   ///< 魔数不是 "NANITE01"
    BadVersion    = 4,   ///< 版本不是 kNaniteFileVersion
    BadIndexCount = 5,   ///< indexCount 不是 3 的倍数（无法按 3×u16 打包）
    Misaligned    = 6,   ///< 某段起点不是 16B 对齐（防御性：改步长时才会触发）
    OutOfBounds   = 7,   ///< 某段偏移 + 长度超出 `size`（越界 / 截断）
};

/// 失败原因的可读名（单测断言与日志共用；返回的字符串是静态常量）
[[nodiscard]] inline const char* NaniteFileErrorName(NaniteFileError error) {
    switch (error) {
        case NaniteFileError::None:          return "None";
        case NaniteFileError::NullData:      return "NullData";
        case NaniteFileError::TooSmall:      return "TooSmall";
        case NaniteFileError::BadMagic:      return "BadMagic";
        case NaniteFileError::BadVersion:    return "BadVersion";
        case NaniteFileError::BadIndexCount: return "BadIndexCount";
        case NaniteFileError::Misaligned:    return "Misaligned";
        case NaniteFileError::OutOfBounds:   return "OutOfBounds";
    }
    return "Unknown";
}

/// 向上取整到 `kNaniteFileAlignment`（段长与段起点都用它）
[[nodiscard]] constexpr u64 NaniteAlignUpFile(u64 value) {
    return (value + (u64)(kNaniteFileAlignment - 1u)) & ~(u64)(kNaniteFileAlignment - 1u);
}

/// 由头部推导段表：成功返回 `None` 并写出 `outLayout`，失败时不改写出参
[[nodiscard]] inline NaniteFileError TryBuildNaniteFileLayout(const NaniteFileHeader& header,
                                                              NaniteFileLayout& outLayout) {
    // 索引编码前提：索引总数必须是 3 的倍数（3×u16 进 u32[2]）
    if ((header.indexCount % kNaniteIndicesPerTriangle) != 0u) {
        return NaniteFileError::BadIndexCount;
    }

    const u64 headerBytes   = (u64)kNaniteFileHeaderBytes;
    const u64 clusterBytes  = NaniteAlignUpFile((u64)header.clusterCount * (u64)kNaniteClusterRecordBytes);
    const u64 vertexBytes   = NaniteAlignUpFile((u64)header.vertexCount  * (u64)kNaniteVertexRecordBytes);
    const u64 triangleCount = (u64)(header.indexCount / kNaniteIndicesPerTriangle);
    const u64 indexBytes    = NaniteAlignUpFile(triangleCount * (u64)kNaniteIndexBytesPerTriangle);
    const u64 materialBytes = NaniteAlignUpFile((u64)header.materialCount * (u64)kNaniteMaterialRecordBytes);
    const u64 lodBytes      = NaniteAlignUpFile((u64)header.lodLevelCount * (u64)kNaniteLodOffsetBytes);

    const u64 clusterOffset  = headerBytes;
    const u64 vertexOffset   = clusterOffset + clusterBytes;
    const u64 indexOffset    = vertexOffset + vertexBytes;
    const u64 materialOffset = indexOffset + indexBytes;
    const u64 lodOffset      = materialOffset + materialBytes;
    const u64 totalBytes     = lodOffset + lodBytes;

    // 32 位宿主上的兜底：u32 计数 × 64B 的累加在 u64 内不会溢出，但收窄到 usize 可能
    if (totalBytes > (u64)std::numeric_limits<usize>::max()) {
        return NaniteFileError::OutOfBounds;
    }

    // 对齐（防御性：各步长都是 16B 的整数倍，正常计数下不可达；单测用属性循环覆盖它）
    if ((clusterOffset % kNaniteFileAlignment) != 0u || (vertexOffset % kNaniteFileAlignment) != 0u ||
        (indexOffset % kNaniteFileAlignment) != 0u || (materialOffset % kNaniteFileAlignment) != 0u ||
        (lodOffset % kNaniteFileAlignment) != 0u) {
        return NaniteFileError::Misaligned;
    }

    NaniteFileLayout layout;
    layout.headerOffset   = 0;
    layout.headerBytes    = (usize)headerBytes;
    layout.clusterOffset  = (usize)clusterOffset;
    layout.clusterBytes   = (usize)clusterBytes;
    layout.vertexOffset   = (usize)vertexOffset;
    layout.vertexBytes    = (usize)vertexBytes;
    layout.indexOffset    = (usize)indexOffset;
    layout.indexBytes     = (usize)indexBytes;
    layout.materialOffset = (usize)materialOffset;
    layout.materialBytes  = (usize)materialBytes;
    layout.lodOffset      = (usize)lodOffset;
    layout.lodBytes       = (usize)lodBytes;
    layout.totalBytes     = (usize)totalBytes;
    layout.triangleCount  = (u32)triangleCount;
    outLayout = layout;
    return NaniteFileError::None;
}

/// 校验一段内存是不是**合法且完整**的 `.nanite` 文件（RHI-free、可单测）。
///
/// 检查：① 指针非空；② 至少能读出 96B 头部；③ 魔数逐字节相等（无 NUL，按 8 字节比）；
/// ④ 版本等于 `kNaniteFileVersion`；⑤ `indexCount` 是 3 的倍数；⑥ 由计数推导的段表
/// 自身合法（含 16B 对齐）；⑦ `size >= layout.totalBytes`（越界 / 截断）。
/// 【尾部】允许 `size > totalBytes`（将来可能追加调试信息），不把额外字节判为非法。
/// 【按值读头部】用 `memcpy` 而不是 `reinterpret_cast`：调用方缓冲不保证 16B 对齐。
[[nodiscard]] inline NaniteFileError ValidateNaniteFile(const void* data, usize size,
                                                        NaniteFileLayout* outLayout = nullptr) {
    if (data == nullptr) return NaniteFileError::NullData;
    if (size < kNaniteFileHeaderBytes) return NaniteFileError::TooSmall;

    NaniteFileHeader header{};
    std::memcpy(&header, data, sizeof(header));

    for (u32 i = 0; i < 8u; ++i) {
        if (header.magic[i] != kNaniteFileMagic[i]) return NaniteFileError::BadMagic;
    }
    if (header.version != kNaniteFileVersion) return NaniteFileError::BadVersion;

    NaniteFileLayout layout;
    const NaniteFileError layoutError = TryBuildNaniteFileLayout(header, layout);
    if (layoutError != NaniteFileError::None) return layoutError;

    if (size < layout.totalBytes) return NaniteFileError::OutOfBounds;

    if (outLayout != nullptr) *outLayout = layout;
    return NaniteFileError::None;
}

/// `ValidateNaniteFile` 的布尔外壳（忽略具体失败原因）
[[nodiscard]] inline bool ValidateNaniteHeader(const void* data, usize size) {
    return ValidateNaniteFile(data, size, nullptr) == NaniteFileError::None;
}

} // namespace he::render
