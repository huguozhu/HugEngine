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
//   任务 10 补齐了**量化与打包的编解码**（见"任务 10"小节与各函数注释）：位置基准裁决
//   （簇 AABB 中心 + 吃满 10 位）、法线八面体 10+10 位、UV unorm16、索引 3×u16、
//   材质 8B 打包/解包；并把 `NaniteTypes.slang`（Slang 镜像）真正建了起来。
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
//     定稿：单轴编码 `raw = clamp(round((v-origin)/range*FULL_SCALE)) + quantBias`，与解码
//     严格互逆；偏置量本身落进顶点记录的 `quantBias` 字段。
//     **任务 10 的二次裁决**：`origin` 由"网格 `bboxMin`"改为"**簇 AABB 中心**"、
//     `FULL_SCALE` 由 511 改为 **1022**（吃满 10 位），见 `NaniteQuantizePositionAxis`。
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
//   [..]                   NaniteMaterialRecord[]  materialCount × 32B（任务 19 起；此前 8B）
//   [..]                   u32[]                   lodLevelCount × 4B
//   每个段的"起点 16B 对齐、长度向上取整到 16B"。文件尾允许有额外字节（不参与校验）。
//
// 【与 Slang 共享】本节的每个结构体都是 C++ 与
//   `Engine/Shader/Shaders/Nanite/NaniteTypes.slang`（**任务 10 建立**，仅供 include、不是
//   shader 入口）之间的**二进制契约**：字段顺序/类型/偏移必须逐位一致（std430 /
//   StructuredBuffer 视角），量化/解码公式也必须逐字一致。**改这里的布局或公式必须同步三处**：
//   ① 本文件；② 设计 §8；③ Slang 镜像。下面的 `static_assert` 是布局这条纪律的编译期钉子；
//   公式这条纪律由两侧互写的指针注释 + 单测钉住（真正的端到端一致要到任务 12 上传读回 /
//   任务 18 shader 解码才能验证，见 `NaniteUpload.h` 的说明）。
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
inline constexpr usize kNaniteMaterialRecordBytes  = 32u;   ///< 材质（任务 19：由 8B 最小扩展为 32B，见下）
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

// ============================================================
// §14.8 任务 10：量化与打包的**编解码函数**（本节的落点一览）
//
// 布局（结构体尺寸/偏移）是任务 7 定的，本任务只补"量化函数"（§8.4 尾注 #13 明确留给任务 10）：
//   · 位置：`NaniteQuantizePositionAxis` / `NaniteDequantizePositionAxis` —— 簇 AABB 中心 +
//     网格最大范围、**吃满 10 位**（任务 10 的基准裁决，替代 §8.4 的 `bboxMin` 口径）；
//   · 法线：`NanitePackNormal` / `NaniteUnpackNormal` —— 八面体 **10+10 位**（x/y 域）；
//   · UV：`NaniteQuantizeUV` / `NaniteDequantizeUV` —— **unorm16**（不用 half）；
//   · 索引：`NanitePackTriangle` / `NaniteTriangleIndex0/1/2`（任务 7 已定稿，本任务只加
//     `IsNaniteTriangleIndexCount` 把"3 个一组"的结构前提显式化）；
//   · 材质：`NaniteMakeMaterialRecord`（任务 19 起 32B：baseColorFactor + metallic/roughness
//     因子 + 纹理掩码 + bindless 纹理基索引；原 8B 的"两个纹理 ID"放不下这些必需字段）。
// 每个函数上方都写了"为什么这么选 + 与 Slang 镜像的同步纪律"；端到端的
// "pack → upload → shader" 一致性要到任务 12（上传读回）与任务 18（shader 解码）才能验证。
// ============================================================

// ── 量化顶点记录（16B；裁决 #7 与 #9）──
//
/// 10 位字段的位宽/掩码与**有符号量化偏置**（§8.4 的解码是 `raw - 512`）
inline constexpr u32 kNaniteVertexQuantBits = 10u;
inline constexpr u32 kNaniteVertexQuantMask = 0x3FFu;
inline constexpr i32 kNaniteVertexQuantBias = 512;
/// 有符号量化值的范围（`raw - bias`）：raw = 0 ⇒ −512，raw = 1023 ⇒ +511
inline constexpr i32 kNaniteVertexQuantMin = -512;
inline constexpr i32 kNaniteVertexQuantMax = 511;
/// 位置量化的**全量程乘数**（任务 10 定稿）：把 `range` 映射到有符号 `[-512, 511]` 用的比例
/// = `max - min - 1` = **1022**（= 2 × 511）。见 `NaniteQuantizePositionAxis` 的基准裁决说明。
inline constexpr i32 kNaniteVertexQuantFullScale = 2 * kNaniteVertexQuantMax;
static_assert(kNaniteVertexQuantFullScale == 1022,
              "全量程必须是 1022（有符号 [-512,511] 的步数），否则位置编解码不再互逆");

/// 量化顶点（`vertexCount × 16B`）
///
/// 【“每簇局部 + 簇心基准”口径（任务 10，§14.19 硬约束①）】`packedPosition` 存的是**相对本簇
///   `boundsCenterRadius.xyz`** 的局部偏移（尺度 = 网格最大范围），不是世界坐标 —— 共享内容
///   （DAG 去重后同一份 `vertexOffset`）因此能被不同位置的簇正确还原。
/// 【为什么有 `quantBias` 而不是 `_pad`】裁决 #7/#9：把量化偏置放进记录内 ⇒ 解码
/// `raw - vertex.quantBias` 不需要任何外部常量表（规则②"自包含"）；代价是每顶点 +4B，
/// 换来的是 16B 对齐 + 编码/解码有唯一落点。
struct alignas(16) NaniteVertex {
    u32 packedPosition = 0;   // 偏移 0：R10G10B10A2：x/y/z 各 10 位有符号（+bias）+ w[31:30]=1
    u32 packedNormal   = 0;   // 偏移 4：八面体 10+10 位（任务 10）落在 x/y 域，z/w 保留写 0
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

/// 单轴位置编码（**任务 10 定稿：盒中心基准 + 吃满 10 位**）
///
/// ```text
/// signed = clamp(round((v - origin) / range × 1022), -512, 511)
/// raw    = clamp10(signed + bias)        // bias = kNaniteVertexQuantBias = 512
/// ```
/// 与 `NaniteDequantizePositionAxis` 严格互逆（`v' = origin + signed / 1022 × range`）。
///
/// 【为什么改基准：任务 10 的裁决，替代任务 7 §8.4 的 `bboxMin` 口径】
///   任务 7 的已知取舍（§8.4 尾注、§14.17）：以 `bboxMin` 为原点、把
///   `[bboxMin, bboxMin+maxExtent]` 映射到有符号 `[0, 511]` ⇒ 盒内顶点只用到 10 位的**上半段**
///   （`raw ∈ [512, 1023]`），负半段 512 个码点永远用不到：等效精度只有 ~9 位
///   （步长 = range/511、往返误差 ≤ range/1022）。
///   任务 10 改为"以盒**中心**为原点、把 `[origin - range/2, origin + range/2]` 映射到有符号
///   `[-512, 511]`（乘数 **1022**）"：同样一个 `range` 下步长从 `range/511` **减半**到
///   `range/1022`，**1024 个码点全部可用**，往返误差上界从 `range/1022` 收到 `range/2044`。
///   这与 §8.4 建议的 "center = (bboxMin+bboxMax)/2、halfExtent = maxExtent/2" 完全等价
///   （`signed/511 × halfExtent == signed/1022 × range`），只是把除法写成一次乘 1022。
///
/// 【会不会 clamp：不会，且可证】
///   调用口径（§14.19 硬约束①）固定为 `origin` = **簇 AABB 中心**（`boundsCenterRadius.xyz`）、
///   `range` = **网格最大范围**（`max(bboxMax-bboxMin)`）。簇是网格的子集 ⇒
///   `|v - origin| ≤ 簇局部半轴长 ≤ meshExtent/2 = range/2` ⇒ `|signed| ≤ 511`，恒落在
///   `[-512, 511]` 内 ⇒ **任何一个顶点都不会被 clamp**。
///   注意 `range` 的**一半**恰好是"任何簇可能达到的最大半轴长"，所以这个基准既吃满满量程、
///   又天然留出 0 的溢出风险 —— 这正是选它而不是"每簇各自半轴长"的理由（后者会引入每簇
///   尺度字段、并破坏 §14.19 的共享内容口径）。
///   函数内仍保留夹取（防御 NaN / 调用方传错口径）；`NanitePositionQuantizeClamps()` 供
///   `PackNaniteClusters()` 统计实际 clamp 次数（测试网格实测 0，见单测 MESSAGE）。
///
/// 【与 Slang 镜像的关系】本函数的公式必须与 `NaniteTypes.slang` 的
///   `naniteQuantizePositionAxis` / `naniteDecodePositionAxis` **逐字一致**
///   （改这里必须同步那边 + 设计 §8）。
/// 【退化轴】`range <= 0`（退化）或 NaN ⇒ 返回 `bias`（等价于该轴取 `origin`）。
[[nodiscard]] inline u32 NaniteQuantizePositionAxis(float v, float origin, float range,
                                                    i32 bias = kNaniteVertexQuantBias) {
    if (!(range > 0.0f)) return NaniteClampRaw10(bias);
    const float normalized = (v - origin) / range;                      // [-0.5,0.5] 表示落在盒内
    const float scaled     = normalized * (float)kNaniteVertexQuantFullScale;   // ×1022 ⇒ [-511,511]
    if (!(scaled == scaled)) return NaniteClampRaw10(bias);             // NaN 兜底
    const float clamped = scaled < (float)kNaniteVertexQuantMin ? (float)kNaniteVertexQuantMin
                        : (scaled > (float)kNaniteVertexQuantMax ? (float)kNaniteVertexQuantMax
                                                                 : scaled);
    return NaniteClampRaw10((i32)std::lround(clamped) + bias);
}

/// 单轴位置解码：与 `NaniteQuantizePositionAxis` 严格互逆（误差 ≤ range/2044 = 半个量化步）
///
/// 【口径】`v = origin + (raw - bias) / 1022 × range`；`origin` / `range` 必须与编码端**同一个**
///   簇的 `boundsCenterRadius.xyz` 与**同一个**网格最大范围（§14.19 硬约束①：共享的
///   `vertexOffset` 让不同位置的簇读到同一份局部坐标，必须靠各自的簇心还原世界位置）。
/// 【退化轴】`range <= 0` ⇒ 返回 `origin`（与编码端的退化分支对称）。
[[nodiscard]] inline float NaniteDequantizePositionAxis(u32 raw, float origin, float range,
                                                        i32 bias = kNaniteVertexQuantBias) {
    const i32 signedValue = (i32)(raw & kNaniteVertexQuantMask) - bias;   // 有符号 SNORM 量化值
    if (!(range > 0.0f)) return origin;
    return origin + (float)signedValue / (float)kNaniteVertexQuantFullScale * range;
}

/// 该顶点在该轴上**会不会被 10 位范围夹住**（诊断用，`PackNaniteClusters` 用它统计 clamp 数）
///
/// 判据与编码端逐个字节对齐：先算未夹取的 `scaled`，再看四舍五入后是否越过
/// `[kNaniteVertexQuantMin, kNaniteVertexQuantMax]`。按任务 10 的调用口径（簇心 + 网格最大范围）
/// 它恒为 false（证明见 `NaniteQuantizePositionAxis`）；这里保留独立实现，是为了让
/// "无 clamp" 这条验收有**可测的读数**而不是只靠推导。
[[nodiscard]] inline bool NanitePositionQuantizeClamps(float v, float origin, float range,
                                                       i32 bias = kNaniteVertexQuantBias) {
    (void)bias;   // 偏置只做平移，不影响是否越界；保留参数以强调与编码端同一套口径
    if (!(range > 0.0f)) return false;
    const float scaled = (v - origin) / range * (float)kNaniteVertexQuantFullScale;
    if (!(scaled == scaled)) return false;   // NaN 走编码端的兜底分支，不算 clamp
    // 先做量级护栏再比较，避免 lround 在极端浮点上越界（scaled 超过 ±512.5 时必然 clamp）
    return scaled < (float)kNaniteVertexQuantMin - 0.5f ||
           scaled > (float)kNaniteVertexQuantMax + 0.5f;
}

// ── 法线：八面体（octahedral）编码（§14.8 任务 10 定稿；§8.4 尾注 #13 留给任务 10 的量）──
//
// 【位宽选择：每分量 **10 位**，落在 `packedNormal` 的 R10G10B10A2 位域 x/y 上；z、w 恒写 0】
//   · 依据：§8.4 已把 `packedNormal` 的**位域**定稿为 R10G10B10A2（x[9:0] y[19:10] z[29:20]
//     w[31:30]），任务 7 明确"只把量化函数留给任务 10"（§8.4 尾注 #13）。八面体只需要 2 个
//     分量，因此落在 x/y 两个 10 位域上；z（10 位）与 w（2 位）**保留写 0**，不改 §8.4 的契约。
//   · 为什么不用"把整个 u32 重解释成 R16G16_SNORM（16+16 位）"：那会改掉 §8.4 的位域表，
//     shader 侧解码也得换一套位运算 —— 三处一致（C++ / Slang / §8）的收益大于 10 位的角
//     精度需求：10 位/轴的最坏角误差实测 < 0.2°（单测 MESSAGE 有实测值），对法线着色
//     （含法线贴图与 TAA）足够。将来若要更高精度，落点是把该字段整体改解释为 R16G16，
//     并同步 C++ / Slang / §8.4 三处（属格式改动，不在本任务）。
//   · 位值口径：10 位按 **UNORM**（0…1023）解释，不用 Vulkan 的 R10G10B10A2_SNORM 采样语义
//     —— 我们走的是手写位域而不是纹理格式，编解码两端用同一套位运算即可（三处一致）。
//   · 角度误差的解析上界（供单测设阈值）：八面体把单位球双射到 [-1,1]²，每轴 10 位 ⇒
//     格距 2/1023；在球面"面心"处角误差 ≈ 半格 ≈ 0.056°，在八面体的棱/角附近放大约 2~4 倍，
//     实测最坏 < 0.2°。单测阈值取 **0.5°**（留 2.5 倍余量）。
inline constexpr u32 kNaniteNormalOctahedralBits = 10u;   ///< 每分量 10 位（= R10G10B10A2 的域宽）
/// 法线八面体编码的**角误差验收阈值**（度）：单测的阈值，也是打包器写进
/// `NanitePackStats::normalAngleErrorBoundDegrees` 的那个数字。10 位/轴的实测最坏 < 0.2°，
/// 故取 0.5°（2.5 倍余量）。
inline constexpr float kNaniteNormalAngleErrorBoundDegrees = 0.5f;

/// `[-1, 1] → 10 位 UNORM`（0…1023）：`round((v+1)/2 × 1023)`；NaN/越界夹到边界
[[nodiscard]] inline u32 NaniteQuantizeUNorm10(float value) {
    if (!(value == value)) return 0u;                                  // NaN：按 -1 处理（只影响非法输入）
    const float scaled = (value + 1.0f) * 0.5f * (float)kNaniteVertexQuantMask;
    const float clamped = scaled < 0.0f ? 0.0f
                        : (scaled > (float)kNaniteVertexQuantMask ? (float)kNaniteVertexQuantMask
                                                                  : scaled);
    return NaniteClampRaw10((i32)std::lround(clamped));
}

/// `10 位 UNORM → [-1, 1]`（与 `NaniteQuantizeUNorm10` 互逆）
[[nodiscard]] inline float NaniteDequantizeUNorm10(u32 raw) {
    return (float)(raw & kNaniteVertexQuantMask) / (float)kNaniteVertexQuantMask * 2.0f - 1.0f;
}

/// 单位法线 → 八面体 2D 坐标（每分量落在 `[-1, 1]`）
///
/// 【公式】先归一化，再取 L1 归一化投影到八面体，z < 0 的半球按标准折叠（"展开"下半球）。
/// 【符号约定】折叠时用 `x >= 0 ? +1 : -1`（**不是** `sign(x)`）：`x == 0` 必须稳定取 +1，
///   否则编解码两端在 0 附近会取到不同的符号（Slang 侧的 `sign(0) = 0` 更会直接把坐标清零）。
/// 【退化】零向量 / NaN ⇒ 输出 `(0, 0)`，解码回来是 `+Z`（往返自洽，见 `NaniteDecodeOctahedral`）。
inline void NaniteEncodeOctahedral(float nx, float ny, float nz, float& outX, float& outY) {
    const float lengthSquared = nx * nx + ny * ny + nz * nz;
    if (!(lengthSquared > 0.0f)) { outX = 0.0f; outY = 0.0f; return; }   // 零向量/NaN ⇒ +Z
    const float invLength = 1.0f / std::sqrt(lengthSquared);
    float x = nx * invLength;
    float y = ny * invLength;
    float z = nz * invLength;

    const float l1 = std::fabs(x) + std::fabs(y) + std::fabs(z);
    x /= l1;
    y /= l1;

    if (z < 0.0f) {
        // 下半球折叠：`n.xy = (1 - |n.yx|) * signNotZero(n.xy)`（注意 abs 的分量是**交换**的）
        const float signX = (x >= 0.0f) ? 1.0f : -1.0f;
        const float signY = (y >= 0.0f) ? 1.0f : -1.0f;
        const float foldedX = (1.0f - std::fabs(y)) * signX;
        const float foldedY = (1.0f - std::fabs(x)) * signY;
        x = foldedX;
        y = foldedY;
    }
    outX = x;
    outY = y;
}

/// 八面体 2D 坐标 → 单位法线（与 `NaniteEncodeOctahedral` 互逆；含反向折叠 + 归一化）
inline void NaniteDecodeOctahedral(float ex, float ey, float& outX, float& outY, float& outZ) {
    float x = ex;
    float y = ey;
    float z = 1.0f - std::fabs(x) - std::fabs(y);
    if (z < 0.0f) {
        const float signX = (x >= 0.0f) ? 1.0f : -1.0f;
        const float signY = (y >= 0.0f) ? 1.0f : -1.0f;
        const float unfoldedX = (1.0f - std::fabs(y)) * signX;
        const float unfoldedY = (1.0f - std::fabs(x)) * signY;
        x = unfoldedX;
        y = unfoldedY;
    }
    const float lengthSquared = x * x + y * y + z * z;
    if (!(lengthSquared > 0.0f)) { outX = 0.0f; outY = 0.0f; outZ = 1.0f; return; }
    const float invLength = 1.0f / std::sqrt(lengthSquared);
    outX = x * invLength;
    outY = y * invLength;
    outZ = z * invLength;
}

/// 法线 → `packedNormal`（八面体 10+10 位进 x/y 域；z、w 恒 0；w 不做 §8.4 的 "=1" 约定）
[[nodiscard]] inline u32 NanitePackNormal(float nx, float ny, float nz) {
    float ex = 0.0f;
    float ey = 0.0f;
    NaniteEncodeOctahedral(nx, ny, nz, ex, ey);
    return NanitePackR10G10B10A2(NaniteQuantizeUNorm10(ex), NaniteQuantizeUNorm10(ey), 0u, 0u);
}

/// `packedNormal` → 单位法线（与 `NanitePackNormal` 互逆）
inline void NaniteUnpackNormal(u32 packed, float& outX, float& outY, float& outZ) {
    NaniteDecodeOctahedral(NaniteDequantizeUNorm10(NaniteUnpackR10G10B10A2(packed, 0u)),
                           NaniteDequantizeUNorm10(NaniteUnpackR10G10B10A2(packed, 1u)),
                           outX, outY, outZ);
}

/// 法线往返的**角度误差**（弧度，调用方传单位向量；解码结果已是单位向量）
///
/// 用 `acos(dot)` 而不是 1-cos 近似：角度阈值（0.5°）下点积已在 0.99996 附近，
/// 用 `1-cos` 会丢有效位。`acos` 的入参夹到 [-1,1] 以免浮点越界产生 NaN。
[[nodiscard]] inline float NaniteNormalAngleErrorRadians(float ax, float ay, float az,
                                                         float bx, float by, float bz) {
    const float dot = ax * bx + ay * by + az * bz;
    const float clamped = dot < -1.0f ? -1.0f : (dot > 1.0f ? 1.0f : dot);
    return std::acos(clamped);
}

// ── UV：unorm16 量化（§14.8 任务 10 定稿；§8.4 尾注 #13 留给任务 10 的量）──
//
// 【为什么是 unorm16 而不是 binary16（half）】
//   · §8.4 已把 `packedUV` 定稿为 **R16G16_UNORM**（u[15:0]、v[31:16]）⇒ 位宽不是自由选择，
//     任务 10 要定的是"同一批 16 位按 UNORM 还是按 half 解释"。
//   · 取 UNORM：① 在 `[0,1]` 上**均匀**，步长恒为 1/65535、往返误差 ≤ 1/131070 ≈ 7.6e-6；
//     binary16 在 `(0.5, 1)` 上的间距是 2^-11 ≈ 4.9e-4（是 unorm16 的 **32 倍**），在 1.0 附近
//     还要浪费一大段尾数；② 与 GPU 的 R16G16_UNORM 采样语义一致（任务 18 软光栅可直接按
//     UNORM 读，不必再转 half）；③ UV 的常规取值域就是 `[0,1]`。
//   · 代价（如实记录）：UNORM 表示不了越界 UV（`>1` 的平铺 / `<0`）。本实现按 §8.4 的口径
//     **clamp 到 [0,1]**，并在 `NanitePackStats::uvClampCount` 里如实报告越界分量数
//     （测试网格实测 0）。若将来要支持平铺 UV，落点是把字段改解释为 R16G16_SNORM / 或把
//     小数/整数部分拆开，属跨 C++/Slang/§8.4 的格式改动，本任务不动。
/// unorm16 的满值（`packedUV` 每分量 16 位）
inline constexpr u32 kNaniteUVQuantMax = 0xFFFFu;

/// `[0,1] → unorm16`：`round(v × 65535)`；NaN 取 0、越界 clamp 到 `[0, 65535]`
[[nodiscard]] inline u32 NaniteQuantizeUV(float value) {
    if (!(value == value)) return 0u;                                  // NaN 兜底
    const float scaled = value * (float)kNaniteUVQuantMax;
    const float clamped = scaled < 0.0f ? 0.0f
                        : (scaled > (float)kNaniteUVQuantMax ? (float)kNaniteUVQuantMax : scaled);
    return (u32)std::lround(clamped) & kNaniteUVQuantMax;
}

/// `unorm16 → [0,1]`（与 `NaniteQuantizeUV` 互逆，误差 ≤ 1/131070 = 半个量化步）
[[nodiscard]] inline float NaniteDequantizeUV(u32 raw) {
    return (float)(raw & kNaniteUVQuantMax) / (float)kNaniteUVQuantMax;
}

/// 该 UV 分量是否**越出 `[0,1]`**（打包时据此统计 clamp 数；NaN 也算越界）
[[nodiscard]] inline bool NaniteUVNeedsClamp(float value) {
    return !(value >= 0.0f && value <= 1.0f);
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

/// 索引总数是否满足"**必须 3 个一组**"的编码前提（= 一个完整的三角形列表）
///
/// 这是 `NanitePackedTriangle`（3×u16 进 `u32[2]`）的结构性前提：落盘时"三角形数 =
/// indexCount / 3"，余数一旦非 0 就无法表示。`TryBuildNaniteFileLayout` 用
/// `NaniteFileError::BadIndexCount` 挡住它，打包器与单测共用本判据。
[[nodiscard]] constexpr bool IsNaniteTriangleIndexCount(usize indexCount) {
    return (indexCount % (usize)kNaniteIndicesPerTriangle) == 0u;
}

// ── 材质记录（32B；字段语义由任务 19 定稿，步长由任务 7/10 的 8B **最小扩展**而来）──
//
// 【为什么必须从 8B 扩到 32B（任务 19 的裁决，先报告后扩展）】
//   §8/任务 7 定稿的 8B 记录只有两个 bindless 纹理 ID。任务 19 的验收是"材质字段与既有
//   GBuffer 路径**逐项可比**"，而 GBuffer 路径（`GBuffer.frag.slang:57-81`）的每个字段都由
//   **三个量**共同决定：
//       albedo    = baseColorFactor.rgb × Sample(BaseColor纹理, uv).rgb
//       metallic  = metallicFactor      × Sample(MetallicRoughness纹理, uv).b
//       roughness = clamp(roughnessFactor × Sample(MetallicRoughness纹理, uv).g, 0.04, 1.0)
//   8B 放不下"两个因子 + 一个 float4 基础色因子 + 纹理存在掩码"这三样必需信息 ⇒ 最小扩展为
//   32B：`float4 baseColorFactor`（16B）+ `metallicFactor`（4）+ `roughnessFactor`（4）
//   + `textureMask`（4）+ `bindlessTextureBase`（4）。16B 对齐、字段全是 4B 对齐的标量，
//   与 Slang 侧 `StructuredBuffer` 的 std430 视角逐字段一致。
//   【如实说明取不到什么】`alphaCutoff`（GBuffer 用它做 `discard`）与 ao/emissive 仍**不在**
//   本条记录里：软光栅本任务**不做 alpha 测试**、也**不写** MRT2（emissive/ao 仍是清屏值）——
//   这是任务 18 划定的边界，任务 19 不扩大（扩大就要动"谁写哪些通道"的契约）。
/// 材质记录：与既有 GBuffer 路径**同源**的 PBR 字段（32B；C++ / Slang 二进制契约）
struct alignas(16) NaniteMaterialRecord {
    float baseColorFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };  // 偏移 0 ：与 GPUObjectData::baseColorFactor 同口径
    float metallicFactor     = 1.0f;   // 偏移 16：金属度因子
    float roughnessFactor    = 1.0f;   // 偏移 20：粗糙度因子
    u32   textureMask        = 0u;     // 偏移 24：纹理存在位掩码（与 ComputeMaterialTextureMask 同一套位）
    u32   bindlessTextureBase = 0u;    // 偏移 28：bindless 纹理基索引（= MeshComponent::materialID）
};

static_assert(sizeof(NaniteMaterialRecord) == kNaniteMaterialRecordBytes,
              "材质记录必须 32B（任务 19 的最小扩展：16B 对齐 + 逐字段与 Slang 一致）");
static_assert(offsetof(NaniteMaterialRecord, baseColorFactor)      == 0,  "baseColorFactor 在偏移 0");
static_assert(offsetof(NaniteMaterialRecord, metallicFactor)       == 16, "metallicFactor 在偏移 16");
static_assert(offsetof(NaniteMaterialRecord, roughnessFactor)      == 20, "roughnessFactor 在偏移 20");
static_assert(offsetof(NaniteMaterialRecord, textureMask)          == 24, "textureMask 在偏移 24");
static_assert(offsetof(NaniteMaterialRecord, bindlessTextureBase)  == 28, "bindlessTextureBase 在偏移 28");

/// 材质记录打包：按字段填一条记录（与既有 GBuffer 路径读的**同一批**字段）
[[nodiscard]] inline NaniteMaterialRecord NaniteMakeMaterialRecord(const float baseColorFactor[4],
                                                                  float metallicFactor,
                                                                  float roughnessFactor,
                                                                  u32 textureMask,
                                                                  u32 bindlessTextureBase) {
    NaniteMaterialRecord record;
    for (u32 i = 0u; i < 4u; ++i) {
        record.baseColorFactor[i] = (baseColorFactor != nullptr) ? baseColorFactor[i] : 1.0f;
    }
    record.metallicFactor      = metallicFactor;
    record.roughnessFactor     = roughnessFactor;
    record.textureMask         = textureMask;
    record.bindlessTextureBase = bindlessTextureBase;
    return record;
}

/// 材质记录的**原始 u32 字**视图（`word` 0..7；越界返回 0）：单测用它核对字节序/偏移，
///   与 Slang 侧的 `uint8` 视角逐位对应。
[[nodiscard]] inline u32 NaniteUnpackMaterial(const NaniteMaterialRecord& record, u32 word) {
    if (word >= 8u) return 0u;
    u32 raw[8];
    std::memcpy(raw, &record, sizeof(raw));
    return raw[word];
}

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

// ============================================================
// §14.8 任务 13：实例剔除（Instance Culling，设计 §5.1 Phase 1）
//
// 【本节的定位与「为什么放这里」】
//   实例剔除的**唯一输入契约**是 `GPUSceneObject`（128B）。那个结构体定义在
//   `Engine/Render/Pipeline/GPUScene.h:26-40`，但那个头会牵入 `RHI/RHI.h` 与
//   `Pipeline/Material.h`，**不能**被 RHI-free 的 `NaniteTypes.h` include（§14.7：
//   Scene 侧要 include 本文件；`Tests/CMakeLists.txt` 也只在 RHI-free 前提上编译本文件）。
//   所以本节放一份**逐字段同布局的 RHI-free 镜像** `NaniteInstanceGpuObject`，
//   并在 `NaniteCull.cpp`（Render 目标，能 include 真身）用 `sizeof` + 逐字段 `offsetof`
//   的 `static_assert` 把它钉死在真身上 —— 契约一旦漂移就编译不过。
//
// 【坐标系与口径（全节统一）】
//   · 实例的 `boundsMin/boundsMax`、包围球球心与视锥平面**全部是世界空间**；
//   · 视锥平面约定与 `Math/Geometry.h` 的 `he::Frustum` **完全一致**：
//     `planes[i] = (n.xyz, d)`，`dot(n, p) + d >= 0` 表示**在内侧**；
//     平面顺序 `[左, 右, 下, 上, 近, 远]`；法线已归一化。
//   · 球与平面的相交判据与 `he::Frustum::Intersects(Sphere)`（`Geometry.cpp:78-87`）
//     逐字一致：`dot(n, c) + d < -radius` ⇒ 该平面外侧 ⇒ 不可见（**不加 epsilon**，
//     与 GPU 侧同一判据，保证 CPU/GPU 逐项一致）。
//
// 【与 §5.1 的关系】本节只做 **Phase 1（Instance Culling）的视锥粗筛**；
//   Hi-Z 遮挡剔除（Phase 1 的第二半）与 Phase 2/3 的簇 BVH/LOD 属任务 14/15，
//   不在本节也不在本任务。
// ============================================================

/// 合成实例网格的容量上限（任务 13 的验收输入规模；`nanite_instance_test_count` 会被钳到它）
inline constexpr u32 kNaniteMaxTestInstances = 256u;

/// 合成实例网格的**默认条数**（cfg 键 `nanite_instance_test_count` 的默认值）
inline constexpr u32 kNaniteDefaultTestInstances = 64u;

/// 合成实例的 AABB 半轴长相对"实例到相机距离"的比例（见 NaniteCull.cpp 的网格生成）。
/// 放在这里是为了让"球从 128B 契约的 bounds 字段推导"这条口径有**唯一常量来源**，
/// 而不是散落在生成代码里的魔法数。
inline constexpr float kNaniteTestInstanceRadiusScale = 0.06f;

// ── GPUSceneObject 的 RHI-free 布局镜像（128B）──
//
/// 【硬契约】与 `Engine/Render/Pipeline/GPUScene.h:26-40` 的 `GPUSceneObject` 逐字段同布局：
///   `localToWorld[0..64)`、`boundsMin[64..80)`、`boundsMax[80..96)`、
///   然后 8 个 u32 依次 `meshIndex/materialIndex/objectID/visibilityFlags/indexCount/`
///   `firstIndex/vertexOffset/_pad`（`[96..128)`）。
/// 【`alignas(16)` 是必需的】`GPUSceneObject` 的首成员是 glm 的 `float4x4`（`GLM_FORCE_DEFAULT_ALIGNED_GENTYPES`
///   ⇒ 16B 对齐），镜像用 `float[16]` 时只有把整个结构体也对齐到 16 才能复现 `64/80` 这两个偏移。
/// 【字段语义】本任务只消费 `boundsMin/boundsMax`（推包围球）与 `indexCount`（空实例跳过）；
///   其余字段原样保留，供任务 14+ 的簇剔除与 GBuffer 材质解析使用。
struct alignas(16) NaniteInstanceGpuObject {
    float localToWorld[16];   // 偏移 0：世界变换（列主序，与 glm 的 mat4 同）
    float boundsMin[4];       // 偏移 64：世界空间 AABB 下界（w 不用）
    float boundsMax[4];       // 偏移 80：世界空间 AABB 上界（w 不用）
    u32   meshIndex;          // 偏移 96
    u32   materialIndex;      // 偏移 100
    u32   objectID;           // 偏移 104
    u32   visibilityFlags;    // 偏移 108
    u32   indexCount;         // 偏移 112：IndirectDraw 的索引数（0 ⇒ 无几何可画，跳过）
    u32   firstIndex;         // 偏移 116
    i32   vertexOffset;       // 偏移 120
    u32   _pad;               // 偏移 124：对齐填充
};

static_assert(sizeof(NaniteInstanceGpuObject) == 128,
              "实例表条目必须 128B（= GPUSceneObject 的 std430 布局）");
static_assert(alignof(NaniteInstanceGpuObject) == 16, "实例表条目必须 16B 对齐（复现 64/80 偏移）");
static_assert(offsetof(NaniteInstanceGpuObject, localToWorld)  == 0,   "localToWorld 必须在偏移 0");
static_assert(offsetof(NaniteInstanceGpuObject, boundsMin)     == 64,  "boundsMin 必须在偏移 64");
static_assert(offsetof(NaniteInstanceGpuObject, boundsMax)     == 80,  "boundsMax 必须在偏移 80");
static_assert(offsetof(NaniteInstanceGpuObject, meshIndex)     == 96,  "meshIndex 必须在偏移 96");
static_assert(offsetof(NaniteInstanceGpuObject, materialIndex) == 100, "materialIndex 必须在偏移 100");
static_assert(offsetof(NaniteInstanceGpuObject, objectID)      == 104, "objectID 必须在偏移 104");
static_assert(offsetof(NaniteInstanceGpuObject, visibilityFlags) == 108, "visibilityFlags 必须在偏移 108");
static_assert(offsetof(NaniteInstanceGpuObject, indexCount)    == 112, "indexCount 必须在偏移 112");
static_assert(offsetof(NaniteInstanceGpuObject, firstIndex)    == 116, "firstIndex 必须在偏移 116");
static_assert(offsetof(NaniteInstanceGpuObject, vertexOffset)  == 120, "vertexOffset 必须在偏移 120");
static_assert(offsetof(NaniteInstanceGpuObject, _pad)          == 124, "_pad 必须在偏移 124");

// ── 每实例包围球（16B）──
//
/// 【为什么单独一张表而不是在 shader 里现推】`center+radius` 由 CPU 侧从 128B 契约的
///   `boundsMin/boundsMax` 推一次（`NaniteSphereFromInstanceBounds`），CPU 参考剔除与 GPU
///   通道**读同一份比特**。若让 GPU 在 shader 里现推（`sqrt` + FMA 收缩），浮点末位差异会在
///   "球正好切在平面上"的实例上翻转可见性，破坏"逐项一致"这条验收 —— 这正是本任务把球显式
///   落成一张表的原因。
struct alignas(16) NaniteInstanceSphere {
    float center[3];   // 偏移 0：世界空间球心
    float radius;      // 偏移 12：半径（≥ 0）
};

static_assert(sizeof(NaniteInstanceSphere) == 16, "包围球必须 16B（StructuredBuffer 步长）");
static_assert(offsetof(NaniteInstanceSphere, center) == 0,  "球心必须在偏移 0");
static_assert(offsetof(NaniteInstanceSphere, radius) == 12, "半径必须在偏移 12");

/// 从 128B 实例条目的 `boundsMin/boundsMax` 推包围球（center = 盒心、radius = 半对角线长）
///
/// 【退化输入】`max < min`（非法 AABB）时对应半轴长取 0（不产生负半径/NaN），
///   与"退化半径"单测的口径一致：半径 0 = 退化成点，仍参与逐平面点测试。
[[nodiscard]] inline NaniteInstanceSphere NaniteSphereFromInstanceBounds(
        const NaniteInstanceGpuObject& instance) {
    NaniteInstanceSphere sphere{};
    float radiusSquared = 0.0f;
    for (u32 axis = 0; axis < 3u; ++axis) {
        const float lo = instance.boundsMin[axis];
        const float hi = instance.boundsMax[axis];
        sphere.center[axis] = (lo + hi) * 0.5f;
        const float halfExtent = (hi > lo) ? (hi - lo) * 0.5f : 0.0f;
        radiusSquared += halfExtent * halfExtent;
    }
    sphere.radius = std::sqrt(radiusSquared);
    return sphere;
}

// ── 视锥六平面（24 个 float，96B）──
//
/// 与 `Math/Geometry.h` 的 `he::Frustum` **同约定、同顺序**的 POD：
///   `planes[i] = (n.xyz, d)`，`dot(n, p) + d >= 0` 在内侧；
///   顺序 `[左, 右, 下, 上, 近, 远]`；法线已归一化（本结构不强制，但提取函数会归一化）。
struct alignas(16) NaniteFrustumPlanes {
    float planes[6][4];
};

static_assert(sizeof(NaniteFrustumPlanes) == 96, "视锥必须 6×float4 = 96B");
static_assert(offsetof(NaniteFrustumPlanes, planes) == 0, "planes 必须在偏移 0");

/// 从**列主序** view-proj（16 个 float，即 glm `mat4` 的 `&m[0][0]` 布局）提取 6 个视锥平面
///
/// 【算法】Gribb/Hartmann 行组合，与 `Engine/Core/Math/Geometry.cpp:12-45` 的
///   `he::Frustum::FromViewProj` **逐字同源**（含 Vulkan `[0,1]` 深度的近平面取 row2、
///   以及"不取反、只按 xyz 长度归一化"）。单测用同一个 view-proj 与 `he::Frustum::FromViewProj`
///   逐平面比对，保证两处不会各说各话。
/// 【列主序索引】元素 `(row r, col c) = viewProjColumnMajor[c * 4 + r]`。
[[nodiscard]] inline NaniteFrustumPlanes NaniteExtractFrustumPlanes(
        const float viewProjColumnMajor[16]) {
    NaniteFrustumPlanes frustum{};
    if (viewProjColumnMajor == nullptr) return frustum;

    const float* m = viewProjColumnMajor;
    // 取 row3 ± rowN 的四个系数（Gribb/Hartmann 行组合法）
    const auto makePlane = [m](u32 row, bool add, float* outPlane) {
        const float sign = add ? 1.0f : -1.0f;
        for (u32 col = 0; col < 4u; ++col) {
            outPlane[col] = m[col * 4u + 3u] + sign * m[col * 4u + row];
        }
    };

    makePlane(0u, true,  frustum.planes[0]);   // 左：  row3 + row0
    makePlane(0u, false, frustum.planes[1]);   // 右：  row3 - row0
    makePlane(1u, true,  frustum.planes[2]);   // 下：  row3 + row1
    makePlane(1u, false, frustum.planes[3]);   // 上：  row3 - row1
    for (u32 col = 0; col < 4u; ++col) {
        frustum.planes[4][col] = m[col * 4u + 2u];   // 近：  row2（Vulkan [0,1]：z >= 0）
    }
    makePlane(2u, false, frustum.planes[5]);   // 远：  row3 - row2

    // 归一化：球-平面距离判据要求法线是单位向量（不取反，保留 Gribb/Hartmann 原始朝向）
    for (u32 i = 0; i < 6u; ++i) {
        const float nx = frustum.planes[i][0];
        const float ny = frustum.planes[i][1];
        const float nz = frustum.planes[i][2];
        const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (length > 1.0e-4f) {
            const float invLength = 1.0f / length;
            for (u32 col = 0; col < 4u; ++col) frustum.planes[i][col] *= invLength;
        }
    }
    return frustum;
}

/// 包围球是否与视锥相交（= 是否可见）：任一面满足 `dot(n, c) + d < -radius` 就不可见
///
/// 【判据】与 `he::Frustum::Intersects(Sphere)`（`Geometry.cpp:78-87`）以及
///   `Nanite_InstanceCull.comp.slang` 的 GPU 实现**完全一致**（同样的比较符、无 epsilon）。
/// 【NaN/退化】`radius` 为负时按 0 处理（退化成点测试）；NaN 半径会走"可见"分支，
///   但生成侧保证不产生 NaN（`NaniteSphereFromInstanceBounds` 用 sqrt 于非负和）。
[[nodiscard]] inline bool NaniteSphereVisibleInFrustum(const NaniteFrustumPlanes& frustum,
                                                       const float center[3],
                                                       float radius) {
    if (!(radius > 0.0f)) radius = 0.0f;
    for (u32 i = 0; i < 6u; ++i) {
        const float distance = frustum.planes[i][0] * center[0]
                             + frustum.planes[i][1] * center[1]
                             + frustum.planes[i][2] * center[2]
                             + frustum.planes[i][3];
        if (distance < -radius) return false;
    }
    return true;
}

// ============================================================
// §14.8 任务 15：三阶段簇剔除的判据与参数（Phase 2 的 Hi-Z 遮挡 + Phase 3 的 LOD 选择）
//
// 【本节为什么放在任务 14 的遍历实现**之前**】任务 15 没有再写一份遍历，而是**扩展**任务 14 的
//   `NaniteTraverseClusterBVHCPU`（新增三个可选输入：Phase 1 的可见实例掩码 / Hi-Z 遮挡 /
//   LOD 选择）。C++ 的 inline 函数必须在调用点之前可见 ⇒ 判据与参数放这里，遍历体在下面。
//
// 【与设计 §5.1 的逐句对应】
//   · Phase 2 后半 "Hi-Z occlusion cull (sample Hi-Z pyramid)" ⇒ `NaniteHiZOccluded`（GPU 侧
//     是 `Nanite_ClusterBVH.comp.slang` 的 `hizOccluded`，两边逐句同构）；
//   · Phase 3 "projectedError = cluster.maxError / distance；selectedLOD = selectLevel(
//     projectedError, threshold = 1 pixel)" ⇒ `NaniteLODErrorTooCoarse` + `NaniteLODClusterSelected`。
//
// 【公式与阈值的核实结论（如实记录一处量纲修正）】§5.1 原文把 `maxError / distance` 记作
//   "projectedError" 并直接与 "threshold = 1 pixel" 比较 —— 但 `maxError / distance` 是**角尺度**
//   （弧度），与"像素"不同量纲，直接比会得到一个与分辨率无关的错误阈值。工程上必须乘**像素焦距**：
//   ```text
//   projectedErrorPixels = maxError / distance × focalPixels
//   focalPixels          = 0.5 × screenH / tan(fovY/2)     // = 半屏高 × 投影矩阵的 m11
//   selectedLOD          = 使 projectedErrorPixels ≤ 1.0 的那一级（见 `NaniteLODClusterSelected`）
//   ```
//   阈值取设计原文的 `kNaniteLODThresholdPixels = 1.0` 像素；`focalPixels` 由
//   `NaniteClusterLODFocalPixels()` 从相机 fov（垂直、度）与屏幕高度算出，**CPU 与 GPU 用同一个
//   数**（在参数缓冲里传同一份比特，避免两端各推一次）。
//   【为什么判据写成乘法】`error × focal > threshold × distance` 与 `error / distance × focal >
//   threshold` 等价，但把除法换成一次乘法 ⇒ 两端各少一次舍入，边界翻转的概率更低。
//
// 【Hi-Z 金字塔的真实接口与采样约定（核实自既有实现；金字塔**资源与口径复用**，构建自建）】
//   · `GPUCulling::BuildHiZPyramid(cmd, screenW, screenH)`（`GPUCulling.cpp:478-543`）；纹理
//     `GPUCulling::GetHiZTexture()`（`GPUCulling.h:97`）、采样器 `GetHiZSampler()`（:95）。
//   · 格式/层级：`R32_FLOAT`、`mipLevels = kHiZMips = 8`（`GPUCulling.h:130`），层数 =
//     `1 + floor(log2(max(w,h)))` 钳到 8（`GPUCulling.cpp:483-487`）。
//   · 内容：每层取 2×2 的**最小深度**（`HiZDownsample.comp.slang:31`）⇒ 层 L 覆盖
//     `2^L × 2^L` 足迹的最近深度；深度是 Vulkan `[0,1]`（近=0、远=1）⇒ `zNear > 采样值`
//     即被遮挡。
//   · **mip0 从未被写入**：既有构建器把源深度下采样进 **mip1**，金字塔纹理的 mip0 没人写
//     ⇒ 遮挡测试的 LOD 下限必须钳到 `kNaniteHiZMinMip = 1`。既有 `GPUCull_TwoPhase.comp.slang:34`
//     把下限钳到 0（会采样未写入的 mip0）—— 这是一处**既有缺陷**，属本任务"只读参考"的范围，
//     不在改动面内（已写入任务 15 的实施记录）。
//   · **构建为什么由模块自己做**（任务 15 的实测裁决）：既有 `BuildHiZPyramid` 在循环里逐 mip
//     更新**同一个**描述符集，而本引擎的 GPU 在**执行期**读取描述符、最后一次主机写对整段命令
//     缓冲生效 ⇒ 那 7 次派发全部用最后一个状态，构建出来的金字塔**全 0**（实测）。因此模块用
//     既有的 `HiZDownsample` 口径另建一份下采样（`Nanite_HiZDownsample.comp.slang`）+
//     "每个目标 mip 一个专属描述符集"，把这条次序依赖从根上避开；`GPUCulling.*` 不在改动面内。
//     完整证据、上游修法与影响面见任务 15 实施记录。
// ============================================================

/// Hi-Z 金字塔的最大层数：**必须**与 `GPUCulling` 的 `kHiZMips`（`GPUCulling.h:130`）一致。
/// 【为什么是镜像常量而不是 include】§14.3 依赖禁令：模块内不得 include `GPUCulling.h`；
///   层数由 `NaniteHiZPyramidMipCount()` 按同一公式重算（单测钉住，见任务 15 实施记录）。
inline constexpr u32 kNaniteMaxHiZMips = 8u;

/// Hi-Z 金字塔**最小可采样层**（= 1，不是 0）：见上"mip0 从未被写入"。
inline constexpr u32 kNaniteHiZMinMip = 1u;

/// LOD 选择的屏幕误差阈值（像素）：设计 §5.1 的 "threshold=1 pixel"（原文数值，未改）。
inline constexpr float kNaniteLODThresholdPixels = 1.0f;

/// LOD 判据里距离的下限：距离为 0/NaN 时投影误差会变成无穷大（所有簇都被判"需要更细"），
///   取一个远小于任何真实距离的正数兜底。CPU 与 GPU 用同一个常量。
inline constexpr float kNaniteLODMinDistance = 1.0e-4f;

/// "选中级别分布"直方图的层数（8）。
/// 【为什么是 8 而不是 `kNaniteMaxLODLevels`(6)】本文件不得 include `NaniteUpload.h`（那会形成
///   Types → Upload 的反向依赖）。取 8 = 2 的幂、且 ≥ 6，单测里对"直方图容得下 6 级"做断言。
inline constexpr u32 kNaniteLODHistogramLevels = 8u;
static_assert(kNaniteLODHistogramLevels >= 6u,
              "直方图必须容得下 kNaniteMaxLODLevels = 6 级（任务 9 的 LOD 链上限）");

/// 每簇的 LOD 元数据（16B；与 `Nanite_ClusterBVH.comp.slang` 的 `LODInfo` 逐字段一致）
///
/// 【三个字段的来处（全部由 `BuildNaniteClusterLODInfo` 从 `.nanite` 簇记录 + LOD 段推出）】
///   · `ownError`    —— **用本簇替代它的全部孩子**渲染时的绝对几何误差 = 孩子记录的
///                      `maxParentLODError`（同一级的孩子共用该级那次简化的误差；叶子 = 0，
///                      因为叶子没有孩子 ⇒ "不再细化"不引入误差）。它回答"本簇够不够好"。
///   · `parentError` —— **用本簇的父簇替代本簇**渲染时的绝对几何误差 = 本簇记录的
///                      `maxParentLODError`（任务 9：级 L → L+1 那次简化的绝对误差；根 = 0）。
///                      它回答"父簇是不是已经够好（那本簇就不该出现）"。
///   · `lodLevel`    —— 本簇所属 LOD 级（0 = 最细；来自 `.nanite` 的 LOD 段，任务 10③ 的
///                      "该级第一个出现簇的下标"，§14.20 明确写了"任务 15 可直接用"）。
///   · `flags` bit0  —— 根簇（没有任何簇以它为子）。**必须显式给出**：根的 `parentError` 也是 0，
///                      若只靠 `parentError == 0` 判断，根会被永远判成"父簇够好"⇒ 一个簇都选不出来。
struct alignas(16) NaniteClusterLODInfo {
    float ownError;      ///< 偏移 0：本簇替代其孩子的绝对误差（叶子 = 0）
    float parentError;   ///< 偏移 4：父簇替代本簇的绝对误差（根 = 0，用 flags 区分）
    u32   lodLevel;      ///< 偏移 8：LOD 级（0 = 最细）
    u32   flags;         ///< 偏移 12：bit0 = 根簇
};

static_assert(sizeof(NaniteClusterLODInfo) == 16, "LOD 元数据必须 16B（StructuredBuffer 步长）");
static_assert(offsetof(NaniteClusterLODInfo, ownError)    == 0,  "ownError 必须在偏移 0");
static_assert(offsetof(NaniteClusterLODInfo, parentError) == 4,  "parentError 必须在偏移 4");
static_assert(offsetof(NaniteClusterLODInfo, lodLevel)    == 8,  "lodLevel 必须在偏移 8");
static_assert(offsetof(NaniteClusterLODInfo, flags)       == 12, "flags 必须在偏移 12");

/// `NaniteClusterLODInfo::flags` 的"根簇"位（其余位保留 0）
inline constexpr u32 kNaniteLODInfoFlagRoot = 1u;

/// Hi-Z 金字塔层数：与 `GPUCulling::BuildHiZPyramid`（`GPUCulling.cpp:483-487`）**同一公式**：
///   `mipCount = 1 + floor(log2(max(w,h)))`，再钳到 `kNaniteMaxHiZMips`。
/// 【为什么要重算而不是问 GPUCulling 要】§14.3 的依赖禁令（不 include `GPUCulling.h`，它也没有
///   公开 `m_HiZMipCount` 的读接口）；公式是纯函数，单测直接钉住若干分辨率下的取值。
[[nodiscard]] inline u32 NaniteHiZPyramidMipCount(u32 screenWidth, u32 screenHeight) {
    const u32 maxDim = (screenWidth > screenHeight) ? screenWidth : screenHeight;
    u32 mipCount = 1u;
    while ((maxDim >> mipCount) >= 2u) ++mipCount;
    if (mipCount > kNaniteMaxHiZMips) mipCount = kNaniteMaxHiZMips;
    return mipCount;
}

/// 像素焦距：`0.5 × screenH / tan(fovY/2)`（= 半屏高 × 投影矩阵的 m11）
///
/// 【口径】`fovYDegrees` 是**垂直**视场角（弧度以外的单位：度），与 `CameraData::fov` 同一个量；
///   与 `glm::perspectiveRH_ZO(radians(fov), ...)` 的 m11 = `1/tan(fovy/2)` 同源。
/// 【退化输入】`screenH <= 0` 或 `tan(fov/2)` 太小（fov ≈ 0/180°）⇒ 返回 0，
///   调用方按"focalPixels == 0 ⇒ 关闭 LOD 选择"处理（`NaniteLODClusterSelected` 的第一条）。
[[nodiscard]] inline float NaniteClusterLODFocalPixels(float screenHeight, float fovYDegrees) {
    if (!(screenHeight > 0.0f)) return 0.0f;
    const float halfFovRadians = fovYDegrees * 0.5f * 3.14159265358979323846f / 180.0f;
    const float t = std::tan(halfFovRadians);
    if (!(t > 1.0e-6f)) return 0.0f;
    return 0.5f * screenHeight / t;
}

/// LOD 判据的谓词："误差 `error`（绝对单位）在距离 `distance` 下投影到屏幕后**粗于**
/// `thresholdPixels` 像素吗"。等价于 `error / distance × focal > threshold`，写成乘法形式。
/// 【NaN/退化】距离按 `kNaniteLODMinDistance` 兜底（`NaN > x` 恒 false ⇒ 走兜底分支）。
[[nodiscard]] inline bool NaniteLODErrorTooCoarse(float error, float distance,
                                                  float focalPixels, float thresholdPixels) {
    const float d = (distance > kNaniteLODMinDistance) ? distance : kNaniteLODMinDistance;
    return error * focalPixels > thresholdPixels * d;
}

/// Phase 3 的 DAG 割判据：本簇是否**该出现在可见列表里**
///
/// 【判据】两条同时成立才选：
///   ① 本簇"替代其孩子"的投影误差 ≤ 阈值 ⇒ 本簇已经够好（不必再细化）；
///   ② **父簇不够好**（父簇替代本簇的投影误差 > 阈值）—— 否则父簇会被选中，本簇出现就是重复。
///   根簇没有父簇 ⇒ ② 自动成立（用 `flags` 的根位，不能用 `parentError == 0`，见结构注释）。
/// 【为什么这样就得到一条"割"】误差沿级单调（任务 9 的 `maxParentLODError` 随级递增）⇒
///   沿每条 DAG 链至多一个交点。父子距离跨过阈值时可能同时选中父子两级 —— 这是**逐簇局部判据**
///   的固有边界（真实 Nanite 用"父不可见则孩子不遍历"的 DAG 遍历消掉它），已在实施记录中如实
///   记录；CPU 与 GPU 用同一判据 ⇒ 不影响"逐簇一致"这条验收。
/// 【关闭路径】`focalPixels <= 0` ⇒ 返回 true（不筛任何簇）——这就是"LOD 选择关闭"的退化口径。
[[nodiscard]] inline bool NaniteLODClusterSelected(const NaniteClusterLODInfo& info,
                                                   float distance, float focalPixels,
                                                   float thresholdPixels) {
    if (!(focalPixels > 0.0f)) return true;   // LOD 选择关闭（退化参数）⇒ 不筛
    if (NaniteLODErrorTooCoarse(info.ownError, distance, focalPixels, thresholdPixels)) {
        return false;   // 本簇还不够好：应交给更细的孩子
    }
    if ((info.flags & kNaniteLODInfoFlagRoot) != 0u) return true;   // 根簇：没有父簇可比
    return NaniteLODErrorTooCoarse(info.parentError, distance, focalPixels, thresholdPixels);
}

/// Hi-Z 采样回调（CPU 参考用）：返回 false = "这个坐标/层采样不到"（保守：不判为被遮挡）。
/// 【为什么要回调】CPU 侧拿不到 Hi-Z 金字塔的逐 texel 内容（RHI 的 `CopyTextureToBuffer` 只读
///   mip0，而 `BuildHiZPyramid` 从不写 mip0）⇒ 生产路径传空采样器（Hi-Z 关闭口径），
///   单测用一个**合成金字塔**的回调把这条判据真正测起来。
using NaniteHiZSampleFn = bool (*)(void* user, float u, float v, u32 mip, float* outDepth);

/// Hi-Z 采样器（CPU 参考用；`sample == nullptr` ⇒ 遮挡测试关闭）
struct NaniteHiZSampler {
    NaniteHiZSampleFn sample = nullptr;   ///< 采样回调
    void*              user   = nullptr;  ///< 回调的用户数据（合成金字塔等）
};

/// 把"世界空间球"投影成**屏幕空间 AABB（UV，未钳制）+ 最近深度**（CPU 侧实现；与 shader
///   `hizOccluded` 的前半段逐句同构）
///
/// 输入：`vpRows` = view-proj 的 **4 个行**（`vpRows[r*4+c]` = 列主序 glm 矩阵的 row r / col c，
///   即 `viewProj[c][r]`）。【为什么按行拆开传】Slang 的 `float4x4` 在内存里的行/列对应依赖编译
///   选项（任务 14 的 shader 注释已记录这一坑）；拆成 4 个 float4 + 显式点积后，CPU 与 GPU 读的
///   是同一份比特、乘的是同一个表达式，风险归零。
/// 输出：`outMinUV/outMaxUV` = 8 个角的屏幕 UV 包围盒（**不钳制**）；`outNearestDepth` = 8 个角的
///   最小 ndc.z（= 最近点；Vulkan `[0,1]`：近 = 0）。
/// 【UV 的 y 方向（P0 修复：与 GPU 侧同一条约定）】本引擎的离屏通道用**负高度视口**
///   （`GBufferRenderer_CPU.cpp:61`），NDC y=+1 落在帧缓冲第 0 行、纹理 v 向下增长 ⇒
///   `v = 0.5 - 0.5*ndc.y`（不是 `ndc.y*0.5+0.5`）。这里与 `Nanite_ClusterBVH.comp.slang` 的
///   `hizOccluded` 保持**同一条**约定；对生产路径的 CPU 参考无行为影响（Hi-Z 恒关闭，且
///   盒尺寸/越屏判据对 y 镜像不变），但单测里的合成金字塔回调因此与 GPU 的 UV 语义一致。
/// 【返回 false 的两种情形（都要求"保守不剔除"）】
///   ① 任一角 `clip.w <= 1e-6`（跨越相机平面 / 在相机之后）—— 投影无意义；
///   ② 任一角出现 NaN/Inf（`!(z > -inf)` 之类的兜底判据）。
[[nodiscard]] inline bool NaniteProjectSphereToScreen(const float vpRows[16],
                                                      const float center[3], float radius,
                                                      float outMinUV[2], float outMaxUV[2],
                                                      float* outNearestDepth) {
    if (vpRows == nullptr || center == nullptr || outMinUV == nullptr || outMaxUV == nullptr
        || outNearestDepth == nullptr) {
        return false;
    }
    float minU = 1.0e30f, minV = 1.0e30f, maxU = -1.0e30f, maxV = -1.0e30f, nearest = 1.0e30f;

    for (u32 corner = 0u; corner < 8u; ++corner) {
        // 角点偏移的位序与 shader / 既有两阶段剔除一致：bit0 = x、bit1 = y、bit2 = z
        const float sx = (corner & 1u) ? radius : -radius;
        const float sy = (corner & 2u) ? radius : -radius;
        const float sz = (corner & 4u) ? radius : -radius;
        const float p[4] = { center[0] + sx, center[1] + sy, center[2] + sz, 1.0f };

        float clip[4];
        for (u32 r = 0u; r < 4u; ++r) {
            clip[r] = vpRows[r * 4u + 0u] * p[0] + vpRows[r * 4u + 1u] * p[1]
                    + vpRows[r * 4u + 2u] * p[2] + vpRows[r * 4u + 3u] * p[3];
        }
        if (!(clip[3] > 1.0e-6f)) return false;   // ① 跨越相机平面 ⇒ 保守

        const float ndcX = clip[0] / clip[3];
        const float ndcY = clip[1] / clip[3];
        const float ndcZ = clip[2] / clip[3];
        if (!(ndcZ == ndcZ)) return false;        // ② NaN 兜底（NaN != NaN）

        const float u = ndcX * 0.5f + 0.5f;
        const float v = 0.5f - ndcY * 0.5f;   // 【P0】负高度视口：v 向下增长 ⇒ y 要翻
        if (u < minU) minU = u;
        if (u > maxU) maxU = u;
        if (v < minV) minV = v;
        if (v > maxV) maxV = v;
        if (ndcZ < nearest) nearest = ndcZ;
    }

    outMinUV[0] = minU; outMinUV[1] = minV;
    outMaxUV[0] = maxU; outMaxUV[1] = maxV;
    *outNearestDepth = nearest;
    return true;
}

/// 选 Hi-Z 层：`ceil(log2(max(1, 屏幕盒最长边像素)))`，再钳到 `[kNaniteHiZMinMip, mipCount-1]`
///
/// 【为什么要按屏幕盒大小选层】层 L 覆盖 `2^L × 2^L` 的足迹：盒子的最长边 ≤ 2^L 像素时，
///   4 个角的采样点各覆盖一个"不小于盒子"的足迹 ⇒ 不会漏掉盒内的遮挡物（保守方向）。
/// 【下限为什么是 1 而不是 0】见 `kNaniteHiZMinMip`（mip0 从未被写入）。
[[nodiscard]] inline u32 NaniteHiZSelectMip(float widthPixels, float heightPixels, u32 hizMipCount) {
    if (hizMipCount <= kNaniteHiZMinMip) return kNaniteHiZMinMip;   // 没有可采样的层
    float largest = (widthPixels > heightPixels) ? widthPixels : heightPixels;
    if (!(largest == largest) || largest < 1.0f) largest = 1.0f;    // NaN / 过小 ⇒ 取 1
    float mip = std::ceil(std::log2(largest));
    if (!(mip == mip) || mip < (float)kNaniteHiZMinMip) mip = (float)kNaniteHiZMinMip;
    const float maxMip = (float)(hizMipCount - 1u);
    if (mip > maxMip) mip = maxMip;
    return (u32)mip;
}

/// Phase 2 后半：**Hi-Z 遮挡测试**（CPU 参考实现；与 shader 的 `hizOccluded` 逐句对应）
///
/// 步骤：① 世界球 → 屏幕 AABB + 最近深度；② 完全在屏幕外 ⇒ 不剔除（Hi-Z 只覆盖已光栅化区域）；
///   ③ 按屏幕盒大小选层；④ 取 4 个角的**最小深度**；⑤ `zNear > 采样最大值` ⇒ 被遮挡。
/// 【关闭路径】`hizMipCount < 2` 或 `sample == nullptr` ⇒ 恒返回 false（不遮挡）——
///   这就是"Hi-Z 关闭"的退化口径，与 GPU 的 `hizMipCount == 0 ⇒ 不测` 同一件事。
[[nodiscard]] inline bool NaniteHiZOccluded(const float vpRows[16],
                                            const float center[3], float radius,
                                            float screenW, float screenH,
                                            u32 hizMipCount, const NaniteHiZSampler& sampler) {
    if (hizMipCount < kNaniteHiZMinMip + 1u || sampler.sample == nullptr) return false;
    if (!(screenW > 0.0f) || !(screenH > 0.0f)) return false;

    float minUV[2], maxUV[2], nearest = 0.0f;
    if (!NaniteProjectSphereToScreen(vpRows, center, radius, minUV, maxUV, &nearest)) return false;

    // 完全在屏幕外（四个不等式全不成立 ⇒ 盒与 [0,1]² 无交）⇒ 保守不剔除
    if (maxUV[0] < 0.0f || maxUV[1] < 0.0f || minUV[0] > 1.0f || minUV[1] > 1.0f) return false;

    const auto clamp01 = [](float v) { return (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v); };
    minUV[0] = clamp01(minUV[0]); minUV[1] = clamp01(minUV[1]);
    maxUV[0] = clamp01(maxUV[0]); maxUV[1] = clamp01(maxUV[1]);

    const float sizeX = (maxUV[0] - minUV[0]) * screenW;
    const float sizeY = (maxUV[1] - minUV[1]) * screenH;
    const u32 mip = NaniteHiZSelectMip(sizeX, sizeY, hizMipCount);

    float d[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (!sampler.sample(sampler.user, minUV[0], minUV[1], mip, &d[0])) return false;
    if (!sampler.sample(sampler.user, maxUV[0], minUV[1], mip, &d[1])) return false;
    if (!sampler.sample(sampler.user, minUV[0], maxUV[1], mip, &d[2])) return false;
    if (!sampler.sample(sampler.user, maxUV[0], maxUV[1], mip, &d[3])) return false;

    float deepest = d[0];
    if (d[1] > deepest) deepest = d[1];
    if (d[2] > deepest) deepest = d[2];
    if (d[3] > deepest) deepest = d[3];
    return nearest > deepest;
}

/// 三阶段剔除链的**可选输入**（默认全关 ⇒ 退化回任务 14 的口径：全部非空实例、纯视锥剔除）
///
/// 【为什么用"一个结构 + 默认值"而不是新写一个函数】任务 14 的遍历体（可见性判据、DFS 顺序、
///   容量口径）一个字节都不用改，任务 15 只是**加三个步骤**；默认值保证任务 14 的单测与调用点
///   继续按原口径工作（不需要改一行既有断言）。
struct NaniteCullChainDesc {
    /// Phase 1 的可见实例**掩码**（`visibleMask[i] != 0` ⇒ 该实例可见）；nullptr ⇒ 不过滤。
    /// 【为什么用掩码而不是"可见列表"】掩码按实例下标寻址 ⇒ 与遍历的实例域
    ///   `[0, min(instanceCount, maxInstances))` 天然对齐，**钳制后的子集是确定的**；而压缩列表
    ///   的槽位顺序由 GPU 原子决定，一旦可见数超过实例域上限，"取前 k 个"就是不确定的子集。
    const u32* visibleMask = nullptr;

    /// Phase 3 的 LOD 元数据（每簇一条，与簇球表同序）；nullptr ⇒ 不做 LOD 选择。
    const NaniteClusterLODInfo* lodInfo = nullptr;

    /// view-proj 的 **4 个行**（`vpRows[r*4+c]` = glm 列主序矩阵的 `viewProj[c][r]`）——Hi-Z 遮挡
    /// 测试要投影世界 AABB 的 8 个角。【为什么按行拆开】见 `NaniteProjectSphereToScreen`。
    /// 全 0（默认）⇒ 投影必然返回 false ⇒ 遮挡测试恒不剔除（安全的退化）。
    float vpRows[16] = { 0.0f, 0.0f, 0.0f, 0.0f,
                         0.0f, 0.0f, 0.0f, 0.0f,
                         0.0f, 0.0f, 0.0f, 0.0f,
                         0.0f, 0.0f, 0.0f, 0.0f };

    float cameraPos[3]      = { 0.0f, 0.0f, 0.0f };  ///< 相机世界坐标（LOD 判据的距离基准）
    float focalPixels       = 0.0f;                  ///< 像素焦距；<= 0 ⇒ 关闭 LOD 选择
    float lodThresholdPixels = kNaniteLODThresholdPixels;  ///< LOD 阈值（像素）

    float screenW = 0.0f;   ///< 屏幕宽（Hi-Z 选层的像素换算）
    float screenH = 0.0f;   ///< 屏幕高
    u32   hizMipCount = 0u; ///< Hi-Z 金字塔层数；< 2 ⇒ 关闭遮挡测试

    /// Hi-Z 采样器（只有 CPU 参考会用到；生产路径传空 ⇒ 关闭，理由见 `NaniteHiZSampleFn`）
    NaniteHiZSampler hiz{};

    /// LOD 选择是否打开（`focalPixels > 0` 且给出了元数据表）
    [[nodiscard]] bool lodEnabled() const { return (focalPixels > 0.0f) && lodInfo != nullptr; }
};

// ============================================================
// §14.8 任务 14：per-instance cluster BVH（节点布局 + CPU 参考深度优先遍历）
//
// 【本节放什么（两处位置的取舍）】
//   · **节点/球/引用的 POD 与 CPU 参考遍历**放在本文件：它们是"与 Slang 共享的 GPU 布局"
//     （shader 侧按本节的逐字段布局镜像）以及"GPU 与 CPU 逐项一致的参考实现"，与任务 13 把
//     `NaniteCullInstancesCPU` 放在这里的做法完全同构；本文件仍是 RHI-free、纯头实现、可单测。
//   · **构建器**（`BuildNaniteClusterBVH`）放在 `NaniteUpload.{h,cpp}`：它消费 `.nanite` 的
//     簇记录（任务 9/10 的产物）、属于"资产 → 加速结构"的构建阶段，与 `PackNaniteClusters`
//     同一层；放在那里也让它天然被 `Tests/TestNaniteBuilder.cpp`（直接编译 NaniteUpload.cpp）
//     覆盖，而本文件不必为了一个构建器引入 `<vector>` 算法。
//
// 【与 §5.1 Phase 2 的对应】"For each visible instance: BVH traverse (cluster tree, depth-first)
//   → Frustum cull cluster bounds"。本任务实现**遍历本身**（构建 + 显式栈 DFS + 视锥球判据）；
//   "Phase 1 的可见实例列表 → Phase 2"这条接线与 Hi-Z 遮挡、LOD 选择一起属任务 15
//   （§14.8 任务 15 的原文就是"三阶段簇剔除 + Hi-Z"）。
//
// 【坐标与判据口径（与任务 13 逐字一致）】簇球的 `center/radius` 是**资产网格空间**（= 任务 9
//   的 `NaniteClusterRecord::boundsCenterRadius`，即合并几何的世界坐标，未施加任何逐物体变换）；
//   实例的平移取自 128B `NaniteInstanceGpuObject::localToWorld` 的**第 4 列**（列主序的
//   元素 12/13/14）。世界球 = 平移 + 网格空间球心、半径不变（本任务的合成实例变换是**纯平移**，
//   见 `NaniteCull.cpp` 的生成代码）。可见性判据 = `NaniteSphereVisibleInFrustum`
//   （任一面 `dot(n,c)+d < -r` ⇒ 不可见，无 epsilon），与 GPU 侧同一表达式。
// ============================================================

/// 每个叶子的簇容量（BVH 的叶子容量）
///
/// 【为什么是 4】每簇的包围球很小（≤64 三角形），叶子放 4 个簇后叶子球仍然足够紧；遍历到叶子
///   后至多 4 次球测试，成本可忽略。代价是树高 ~log2(N/4)：本仓库实测资产 8287 簇 ⇒ 深度 13 级，
///   对显式栈（32）留有充足余量。
inline constexpr u32 kNaniteBVHLeafCapacity = 4u;

/// BVH 的**深度硬上限**（节点数，根节点深度 = 1）
///
/// 【为什么必须有它】中点分裂在极端分布下可能一次只切掉 1 个簇 ⇒ 递归深度退化。超上限即
///   停止分裂（该结点变成"更大的叶子"），把"显式栈会不会溢出"从**运行期风险**变成**构建期
///   不变量**：单测直接断言 `depth <= kNaniteBVHMaxDepth`。
inline constexpr u32 kNaniteBVHMaxDepth = 24u;

/// GPU 显式栈的深度上界（**必须 ≥ `kNaniteBVHMaxDepth`**）
///
/// 【为什么是 32 而不是 24】构建期的深度上界是"节点数"，而 DFS 栈里同时存在的条目数不会超过
///   树高（每层至多压一个"右兄弟"），故 24 其实够用；这里多留 8 层余量，让"栈容量"与"树的
///   深度"这两件事各自有余量、不会因为将来调 `kNaniteBVHMaxDepth` 而互相踩到。
inline constexpr u32 kNaniteBVHMaxStackDepth = kNaniteBVHMaxDepth + 8u;
static_assert(kNaniteBVHMaxStackDepth >= kNaniteBVHMaxDepth, "显式栈必须至少能装下最大深度");

/// "没有这个孩子"的哨兵（叶子节点的 `right`）
inline constexpr u32 kNaniteBVHNoChild = 0xFFFFFFFFu;

/// Phase 2 遍历的**实例域上限**（可见簇引用表的容量 = 它 × 簇数上限）
///
/// 【为什么要有上限】可见簇引用表必须**一次分配、容量恒定**（GPU 原子压缩写入不能中途扩容），
///   而它的最坏规模 = 实例数 × 簇数。把实例域钳到 64：默认配置（`nanite_instance_test_count`
///   = 64）正好用满，而 64 × 8287（本仓库实测资产）= 53 万条引用 < 1M 的容量 ⇒ **永不截断**
///   （截断会让"集合逐项比较"失去意义）。CPU 参考与 GPU 用**同一个钳制口径**。
inline constexpr u32 kNaniteMaxBVHInstances = 64u;

/// BVH 覆盖的**簇数上限**（`NaniteCull::SetClusterBVH` 按它截断并告警）
///
/// 见上：可见簇引用表的大小 = `kNaniteMaxBVHInstances × kNaniteMaxBVHClusters`。本仓库实测资产
/// 8287 簇，16384 留了近一倍余量；超出时**如实告警**并只对前 16384 个簇建 BVH（不静默）。
/// 节点数上界 = 2 × 簇数 - 1（满二叉树），故节点表按 `2 × 16384` 条分配。
inline constexpr u32 kNaniteMaxBVHClusters = 16384u;

/// 可见簇引用表容量（= 实例域上限 × 簇数上限）
inline constexpr u32 kNaniteMaxVisibleClusterRefs =
    kNaniteMaxBVHInstances * kNaniteMaxBVHClusters;

/// BVH 节点表容量（满二叉树的上界：叶子 ≤ 簇数 ⇒ 节点 ≤ 2 × 簇数 - 1）
inline constexpr u32 kNaniteMaxBVHNodes = 2u * kNaniteMaxBVHClusters;

/// BVH 节点（32B；与 `Nanite_ClusterBVH.comp.slang` 的 `BVHNode` 逐字段一致）
///
/// ```text
/// 偏移  0：float4 centerRadius   —— center.xyz = 网格空间包围球心，w = 半径
/// 偏移 16：uint4  link           —— x = left，y = right，z = count，w = flags
///                                   内部节点：left/right = 左右孩子下标，count = 2，flags 无 leaf 位
///                                   叶子节点：left = 叶子簇表首下标，right = kNaniteBVHNoChild，
///                                             count = 簇数（≥1），flags bit0 = 1
/// ```
/// 【为什么用"左/右孩子显式下标"而不是"右孩子 = 左 + 1"】后一种要求两个孩子**下标相邻**，
///   而自顶向下的递归构建无法保证（左子树会吃掉中间的下标）；显式写两个下标只多 4B，
///   换掉一整类"布局约束"，也让 shader 的压栈只需读一个 uint4。
/// 【`flags` 只定义 bit0】其余位保留写 0，避免"某些位有语义但没人知道"的隐患。
struct alignas(16) NaniteBVHNode {
    float center[3];   ///< 偏移 0：网格空间包围球心
    float radius;      ///< 偏移 12：包围球半径（≥ 0）
    u32   left;        ///< 偏移 16：内部节点 = 左孩子下标；叶子 = 叶子簇表首下标
    u32   right;       ///< 偏移 20：内部节点 = 右孩子下标；叶子 = kNaniteBVHNoChild
    u32   count;       ///< 偏移 24：内部节点 = 2；叶子 = 该叶子的簇数（≥ 1）
    u32   flags;       ///< 偏移 28：bit0 = 1 表示叶子
};

static_assert(sizeof(NaniteBVHNode) == 32, "BVH 节点必须 32B（= float4 + uint4）");
static_assert(alignof(NaniteBVHNode) == 16, "BVH 节点必须 16B 对齐（std430 的 float4 步长）");
static_assert(offsetof(NaniteBVHNode, center) == 0,  "center 必须在偏移 0");
static_assert(offsetof(NaniteBVHNode, radius) == 12, "radius 必须在偏移 12");
static_assert(offsetof(NaniteBVHNode, left)   == 16, "left 必须在偏移 16");
static_assert(offsetof(NaniteBVHNode, right)  == 20, "right 必须在偏移 20");
static_assert(offsetof(NaniteBVHNode, count)  == 24, "count 必须在偏移 24");
static_assert(offsetof(NaniteBVHNode, flags)  == 28, "flags 必须在偏移 28");

/// `NaniteBVHNode::flags` 的叶子位（其余位保留 0）
inline constexpr u32 kNaniteBVHNodeFlagLeaf = 1u;

/// 该节点是不是叶子（与 shader 侧的 `(link.w & 1u) != 0` 逐字对应）
[[nodiscard]] inline bool NaniteBVHNodeIsLeaf(const NaniteBVHNode& node) {
    return (node.flags & kNaniteBVHNodeFlagLeaf) != 0u;
}

/// 每簇一个 16B 包围球（**网格空间**；与 `Nanite_ClusterBVH.comp.slang` 的 `ClusterSphere` 一致）
///
/// 【为什么单独一张表而不是让 shader 读 64B 的簇记录】① 遍历只需要 `boundsCenterRadius` 这
///   16B，专门的紧凑表让 GPU 的访存步长与缓存占用都小 4 倍；② 这张表由 CPU 从
///   `NaniteClusterRecord::boundsCenterRadius` **逐位搬运**，CPU 参考遍历读的是**同一份比特**
///   （与任务 13 把实例包围球落成表同一个理由：不让 GPU 现推 sqrt/FMA 的末位差异翻转边界可见性）。
struct alignas(16) NaniteClusterSphere {
    float center[3];   ///< 偏移 0：网格空间球心
    float radius;      ///< 偏移 12：半径（≥ 0）
};

static_assert(sizeof(NaniteClusterSphere) == 16, "簇包围球必须 16B（StructuredBuffer 步长）");
static_assert(offsetof(NaniteClusterSphere, center) == 0,  "球心必须在偏移 0");
static_assert(offsetof(NaniteClusterSphere, radius) == 12, "半径必须在偏移 12");

/// 一条可见簇引用（8B；与 shader 侧的 `ClusterRef` 一致）
///
/// 【为什么带 instance】Phase 2 是 per-instance 的：同一个簇下标会被同一份资产的不同实例
///   （平移副本）分别引用 ⇒ "可见簇集合"的元素必须是 (实例, 簇) 二元组，否则多个实例的可见
///   结果会被错误地折叠成一个集合。
struct alignas(4) NaniteVisibleClusterRef {
    u32 instance;   ///< 偏移 0：实例下标（128B 实例表的条目）
    u32 cluster;    ///< 偏移 4：簇下标（`NaniteClusterRecord` 的下标）
};

static_assert(sizeof(NaniteVisibleClusterRef) == 8, "可见簇引用必须 8B（StructuredBuffer 步长）");
static_assert(offsetof(NaniteVisibleClusterRef, instance) == 0, "instance 必须在偏移 0");
static_assert(offsetof(NaniteVisibleClusterRef, cluster)  == 4, "cluster 必须在偏移 4");

/// BVH 的只读视图（CPU 参考遍历的输入；不含所有权，故本结构仍是纯 POD）
struct NaniteClusterBVHView {
    const NaniteBVHNode*       nodes              = nullptr;  ///< 节点表（[0, nodeCount)）
    u32                        nodeCount          = 0u;       ///< 节点数（0 ⇒ 空 BVH）
    const u32*                 leafClusterIndices = nullptr;  ///< 叶子簇表（扁平；叶子用 [left, left+count)）
    const NaniteClusterSphere* clusterSpheres     = nullptr;  ///< 每簇包围球（网格空间）
    u32                        clusterCount       = 0u;       ///< 簇数（= 簇球表条数）
};

/// CPU 参考遍历的读数（验收要的每个数字都在这里）
struct NaniteClusterBVHTraversalStats {
    u32 visitedNodes      = 0u;  ///< 被访问（测试过）的节点数 —— 与 GPU 的 visited 计数同义
    u32 visibleClusters   = 0u;  ///< 被接受的簇引用总数（**未按输出容量截断**，与 GPU 计数同义）
    u32 stackOverflows    = 0u;  ///< 显式栈放不下的次数（构建期上界保证恒 0；非 0 即上界失效）
    u32 traversedInstances = 0u; ///< 真正参与遍历的实例数（indexCount != 0 **且**通过 Phase 1 掩码）

    // ── 【任务 15】三阶段读数（默认 0；不传 `NaniteCullChainDesc` 时这些字段天然保持 0）──
    u32 frustumPassClusters = 0u;  ///< Phase 2 前半：通过簇球视锥测试的引用数（**未做 Hi-Z**）
    u32 occludedClusters    = 0u;  ///< Phase 2 后半：被 Hi-Z 判为遮挡而剔除的引用数
    u32 lodRejectedClusters = 0u;  ///< Phase 3：被 DAG 割判据筛掉的引用数（需要更细/父簇已够好）
    /// Phase 3 的"选中级别分布"：直方图 `lodHistogram[L] = 被选中的 L 级簇引用数`
    /// （长度固定为 `kNaniteLODHistogramLevels`，越界级计到最后一个槽，见遍历体的防御分支）
    u32 lodHistogram[kNaniteLODHistogramLevels] = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
};

/// **CPU 参考的 per-instance cluster BVH 深度优先遍历**（§14.8 任务 14 的验收基准）
///
/// 输入：
///   · `frustum`       —— 6 个世界空间平面（`NaniteExtractFrustumPlanes(viewProj)` 得到）；
///   · `bvh`           —— 节点表 + 叶子簇表 + 簇球表（GPU 侧读的是同一份比特）；
///   · `instances`     —— 128B `NaniteInstanceGpuObject` 表（只读 `localToWorld` 的平移列与
///                        `indexCount`）；
///   · `instanceCount` / `maxInstances` —— 实例域 = `min(instanceCount, maxInstances)`（与 GPU 的
///                        钳制口径一致：可见簇引用表按 `maxInstances × clusterCount` 分配）；
///   · `outVisible` / `outCapacity` —— 输出可见簇引用（写入前 `outCapacity` 条；**计数不受容量影响**）。
///   · `chain`（【任务 15】可选，默认空 = 任务 14 的原口径）—— Phase 1 的可见实例掩码、Hi-Z 遮挡
///     与 Phase 3 的 LOD 选择。三段判据与 GPU 侧逐句对应：
///       ① Phase 1：`chain.visibleMask[i] == 0` ⇒ 跳过该实例（**实例级**过滤，来自
///          `Nanite_InstanceCull` 的可见性掩码）；
///       ② Phase 2：簇球通过视锥后先 `frustumPassClusters += 1`，若 `NaniteHiZOccluded` 为真则
///          `occludedClusters += 1` 并跳过；
///       ③ Phase 3：`NaniteLODClusterSelected` 为假 ⇒ `lodRejectedClusters += 1` 并跳过，
///          否则 `lodHistogram[lodLevel] += 1` 再写入可见列表。
/// 输出：返回值 = 可见簇引用总数（**未截断**）；`outStats`（可空）填读数。
///
/// 【与 GPU 通道逐条对应（`Nanite_ClusterBVH.comp.slang`）】
///   ① `bvh.nodes == nullptr || nodeCount == 0` ⇒ 0（没有 BVH 就没有遍历）；
///   ② `instances == nullptr || instanceCount == 0 || maxInstances == 0` ⇒ 0；
///   ③ 逐实例：`indexCount == 0` ⇒ 跳过（"空实例无可画几何"，与任务 13 同规则）；
///   ④ 每个实例**独立**从根做一次 DFS（per-instance），显式栈、先压右再压左 ⇒ 左子树先访问；
///   ⑤ 每弹出一个节点即 `visitedNodes += 1`（**先计数、后判可见**：与 GPU 的 `++visited` 同位置）；
///   ⑥ 节点球不可见 ⇒ 整棵子树跳过（节点球是其所有后代簇球的保守并集）；
///   ⑦ 叶子：对 `[left, left+count)` 的每个簇做球测试，可见则过三阶段判据并写入（容量内）。
///
/// 【确定性】不含随机数、不读时间、不并行；同一输入两次调用逐位一致。GPU 侧唯一的非确定性是
///   "原子取槽位"决定可见引用的**写入顺序**，故比较口径是**排序后的逐项相等**（集合等价），
///   与任务 13 相同。
/// 【空/退化输入】空 BVH、空实例表、0 容量、空指针一律返回 0（不崩、不写越界）。
[[nodiscard]] inline u32 NaniteTraverseClusterBVHCPU(
        const NaniteFrustumPlanes& frustum,
        const NaniteClusterBVHView& bvh,
        const NaniteInstanceGpuObject* instances,
        u32 instanceCount,
        u32 maxInstances,
        NaniteVisibleClusterRef* outVisible,
        u32 outCapacity,
        NaniteClusterBVHTraversalStats* outStats,
        const NaniteCullChainDesc& chain = NaniteCullChainDesc{}) {
    if (outStats != nullptr) *outStats = NaniteClusterBVHTraversalStats{};
    if (bvh.nodes == nullptr || bvh.nodeCount == 0u) return 0u;
    if (instances == nullptr || instanceCount == 0u || maxInstances == 0u) return 0u;

    const u32 domain = (instanceCount < maxInstances) ? instanceCount : maxInstances;

    u32 written = 0u;   // 可见簇引用总数（未截断）
    NaniteClusterBVHTraversalStats stats{};

    // 显式栈（固定大小、无分配）：与 GPU 的 `uint stack[kNaniteBVHMaxStackDepth]` 同构。
    // DFS 栈里同时存在的条目数不超过树高，故 `kNaniteBVHMaxDepth` 是它的构造性上界。
    u32 stack[kNaniteBVHMaxStackDepth];

    for (u32 instance = 0u; instance < domain; ++instance) {
        if (instances[instance].indexCount == 0u) continue;   // ③ 空实例跳过
        // 【任务 15】Phase 1 的可见实例掩码（实例级过滤；不给掩码时按任务 14 口径处理全部实例）
        if (chain.visibleMask != nullptr && chain.visibleMask[instance] == 0u) continue;
        ++stats.traversedInstances;

        // 实例变换的平移列（列主序：localToWorld[12..14]）；本任务的合成实例是纯平移。
        const float origin[3] = {
            instances[instance].localToWorld[12],
            instances[instance].localToWorld[13],
            instances[instance].localToWorld[14],
        };

        u32 stackSize = 0u;
        stack[stackSize++] = 0u;   // 根节点恒为 0（构建器保证 DFS 布局下根先分配）

        while (stackSize > 0u) {
            const u32 nodeIndex = stack[--stackSize];
            if (nodeIndex >= bvh.nodeCount) continue;   // 防御：非法孩子下标不越界
            ++stats.visitedNodes;                       // ⑤ 先计数、后判可见

            const NaniteBVHNode& node = bvh.nodes[nodeIndex];
            const float nodeCenter[3] = {
                node.center[0] + origin[0],
                node.center[1] + origin[1],
                node.center[2] + origin[2],
            };
            if (!NaniteSphereVisibleInFrustum(frustum, nodeCenter, node.radius)) {
                continue;   // ⑥ 节点球不可见 ⇒ 整棵子树跳过
            }

            if (NaniteBVHNodeIsLeaf(node)) {
                if (bvh.leafClusterIndices == nullptr || bvh.clusterSpheres == nullptr) continue;
                const u32 first = node.left;
                for (u32 k = 0u; k < node.count; ++k) {
                    const u32 cluster = bvh.leafClusterIndices[first + k];
                    if (cluster >= bvh.clusterCount) continue;   // 防御：越界簇下标不读球表
                    const NaniteClusterSphere& sphere = bvh.clusterSpheres[cluster];
                    const float clusterCenter[3] = {
                        sphere.center[0] + origin[0],
                        sphere.center[1] + origin[1],
                        sphere.center[2] + origin[2],
                    };
                    if (!NaniteSphereVisibleInFrustum(frustum, clusterCenter, sphere.radius)) {
                        continue;
                    }
                    // ── 【任务 15】Phase 2 前半：通过视锥（计数位置与 GPU 的 InterlockedAdd 同位）──
                    ++stats.frustumPassClusters;

                    // ── 【任务 15】Phase 2 后半：Hi-Z 遮挡（`hizMipCount < 2` ⇒ 关闭，恒不遮挡）──
                    if (NaniteHiZOccluded(chain.vpRows, clusterCenter, sphere.radius,
                                          chain.screenW, chain.screenH,
                                          chain.hizMipCount, chain.hiz)) {
                        ++stats.occludedClusters;
                        continue;
                    }

                    // ── 【任务 15】Phase 3：DAG 割（LOD 选择）──
                    if (chain.lodEnabled()) {
                        const float dx = clusterCenter[0] - chain.cameraPos[0];
                        const float dy = clusterCenter[1] - chain.cameraPos[1];
                        const float dz = clusterCenter[2] - chain.cameraPos[2];
                        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                        // 越界簇下标已经在上面的 `cluster >= clusterCount` 里挡掉；但元数据表的
                        // 长度由调用方保证 == clusterCount（`BuildNaniteClusterLODInfo` 的输出）。
                        const NaniteClusterLODInfo& info = chain.lodInfo[cluster];
                        if (!NaniteLODClusterSelected(info, distance, chain.focalPixels,
                                                      chain.lodThresholdPixels)) {
                            ++stats.lodRejectedClusters;
                            continue;
                        }
                        const u32 level = (info.lodLevel < kNaniteLODHistogramLevels)
                                        ? info.lodLevel : (kNaniteLODHistogramLevels - 1u);
                        ++stats.lodHistogram[level];
                    }

                    if (outVisible != nullptr && written < outCapacity) {
                        outVisible[written].instance = instance;
                        outVisible[written].cluster  = cluster;
                    }
                    ++written;
                }
            } else {
                // ④ 先压右、再压左 ⇒ 下一次弹出的是左孩子（深度优先、顺序确定）
                if (stackSize + 2u <= kNaniteBVHMaxStackDepth) {
                    stack[stackSize++] = node.right;
                    stack[stackSize++] = node.left;
                } else {
                    ++stats.stackOverflows;   // 构建期上界失效（正常恒 0）
                }
            }
        }
    }

    stats.visibleClusters = written;
    if (outStats != nullptr) *outStats = stats;
    return written;
}

/// **CPU 参考实例剔除**（§14.8 任务 13 的验收基准；RHI-free、可单测）
///
/// 输入（全部世界空间）：
///   · `frustum`   —— 由 `NaniteExtractFrustumPlanes(viewProj)` 得到（或直接构造 6 平面）；
///   · `instances` —— 128B `GPUSceneObject` 布局的实例表（只读 `indexCount` 与本函数无关的字段）；
///   · `spheres`   —— 每实例包围球（与实例表同序、同个数）；
///   · `instanceCount` / `outVisibleIndices` / `outCapacity`。
/// 输出：可见实例的**升序**下标写入 `outVisibleIndices`，返回值 = 写入个数。
///
/// 【与 GPU 通道同规则（逐条对应 `Nanite_InstanceCull.comp.slang`）】
///   ① `spheres == nullptr || instances == nullptr` ⇒ 返回 0；
///   ② `instances[i].indexCount == 0` ⇒ 跳过（"空实例无可画几何"，GPU 同规则）；
///   ③ `NaniteSphereVisibleInFrustum` 为假 ⇒ 跳过；
///   ④ 输出容量满（`written == outCapacity`）⇒ 立刻停止（GPU 侧对应"可见列表容量上限"；
///      本任务的验收容量 ≥ 实例数，故正常运行不会截断）。
///
/// 【顺序】可见下标按 `i` 升序紧凑写入 —— GPU 侧用原子槽位压缩，**顺序不定**，
///   所以 `LogCull3Readback` 比较前会把 GPU 列表排序（见 `NaniteRenderer.cpp`）。
[[nodiscard]] inline u32 NaniteCullInstancesCPU(const NaniteFrustumPlanes& frustum,
                                                const NaniteInstanceGpuObject* instances,
                                                const NaniteInstanceSphere* spheres,
                                                u32 instanceCount,
                                                u32* outVisibleIndices,
                                                u32 outCapacity) {
    if (instances == nullptr || spheres == nullptr || outVisibleIndices == nullptr) return 0u;
    if (outCapacity == 0u) return 0u;

    u32 written = 0u;
    for (u32 i = 0; i < instanceCount && written < outCapacity; ++i) {
        if (instances[i].indexCount == 0u) continue;   // ② 空实例不参与（与 GPU 同规则）
        if (!NaniteSphereVisibleInFrustum(frustum, spheres[i].center, spheres[i].radius)) {
            continue;                                  // ③ 视锥外
        }
        outVisibleIndices[written++] = i;              // ④ 升序紧凑写入
    }
    return written;
}

// ============================================================
// §14.8 任务 16：可见簇列表 → 间接绘制参数（把 `u_VisibleClusters` 接到光栅端）
//
// 【任务 16 要补的缺口】任务 14/15 把可见簇引用（`NaniteVisibleClusterRef`）算出来了，但**没有
//   消费者**：光栅端（`Nanite_Raster`）当时消费的是任务 3 那条"假簇链"（6 条固定命令）。
//   本节的三个纯函数就是那段缺失的映射 —— 每条可见簇引用 → 一条
//   `VkDrawIndexedIndirectCommand`：
//
//     indexCount    = 簇内**索引个数** = `triangleCount × 3`
//     firstIndex    = 簇在打包索引段里的**首个索引位置** = `triangleOffset × 3`
//     vertexOffset  = 簇的**顶点段起始记录下标** = `vertexOffset`（原样搬运）
//     instanceCount = 1（一个簇一次绘制）
//     firstInstance = **簇号**（`.nanite` 簇表里的下标）—— 光栅端用 `SV_InstanceID` 收它，
//                     用于"这条命令归属哪个簇"的调试观察与逐条比对
//
// 【为什么 firstIndex 是 `triangleOffset × 3`（索引位置）而不是字节偏移】
//   打包索引段是 **3×u16 进 u32[2] = 8B/三角形**（任务 7 的裁决 #6）：每条三角形占 3 个 u16
//   **索引位置**（第 4 个 u16 是填充）。因此"簇的首个索引位置"= `triangleOffset × 3`，与索引
//   宽度无关；`indexCount = triangleCount × 3` 与之同量纲。CPU 与 GPU 两侧都用这一个定义。
//
// 【"无空转"的口径（本任务的核心）】
//   ① 计数与命令由**同一次派发**写出：`Nanite_ClusterBVH.comp.slang` 在同一个原子槽位
//      `slot` 上同时写可见簇引用与间接命令，并只在**接受**该簇时把绘制计数 +1；
//   ② 绘制只覆盖 `[0, count)`（`DrawIndexedIndirectCount`），count 由同一次派发写出 ⇒
//      不存在"命令没写就画"或"命令写了却没画"；
//   ③ 每帧的绘制计数**在命令缓冲内**清零（常驻 0 源 + 4B 拷贝，任务 13/15 的修法），
//      因此不存在"残留上一帧命令"的窗口（主机写清零会与派发竞争，任务 13 实测错读成两倍）。
//
// 【与 Slang 侧的布局契约】下面的两个结构体必须与
//   `Engine/Shader/Shaders/Nanite/Nanite_ClusterBVH.comp.slang` 的 `ClusterDrawRange` /
//   `IndirectCmd` 逐字段一致（那边用 `std430` 步长，这边用 static_assert 钉住）。
// ============================================================

/// 每簇的间接绘制参数（16B；`SetClusterBVH` 一次性上传的只读表）
///
/// 【为什么单独一张表而不是让 shader 读 64B 的簇记录】与任务 14 的簇球表同一个理由：遍历只需要
///   这三个 u32，紧凑表让访存步长小 4 倍；而且这张表由 CPU 从 `NaniteClusterRecord` **逐位搬运**
///   （同一个纯函数 `NaniteMakeClusterDrawRange`），CPU 参考打包与 GPU 打包读的是**同一份比特**。
struct alignas(16) NaniteClusterDrawRange {
    u32 firstIndex   = 0u;    ///< 偏移 0 ：簇在索引段里的首个**索引位置**（= triangleOffset × 3）
    u32 indexCount   = 0u;    ///< 偏移 4 ：簇内索引个数（= triangleCount × 3；3 的倍数）
    i32 vertexOffset = 0;     ///< 偏移 8 ：簇的顶点段起始**记录下标**（原样搬运给命令）
    u32 _pad         = 0u;    ///< 偏移 12：填充到 16B（std430 下与 uint4 步长一致）
};

static_assert(sizeof(NaniteClusterDrawRange) == 16,
              "绘制参数必须 16B（StructuredBuffer 步长）");
static_assert(offsetof(NaniteClusterDrawRange, firstIndex)   == 0,  "firstIndex 必须在偏移 0");
static_assert(offsetof(NaniteClusterDrawRange, indexCount)   == 4,  "indexCount 必须在偏移 4");
static_assert(offsetof(NaniteClusterDrawRange, vertexOffset) == 8,  "vertexOffset 必须在偏移 8");
static_assert(offsetof(NaniteClusterDrawRange, _pad)         == 12, "_pad 必须在偏移 12");

/// 间接命令缓冲的容量（= 可见簇引用表容量）
///
/// 【为什么两者同容量】命令与引用按**同一个原子槽位** `slot` 写入 ⇒ 槽位 k 的引用与槽位 k 的
///   命令永远是同一条簇的记录，读回比对不需要任何映射；容量不同会引入"引用有、命令没有"的
///   中间态，让"逐条比对"失去意义。
inline constexpr u32 kNaniteMaxIndirectDraws = kNaniteMaxVisibleClusterRefs;

/// 占位索引缓冲的**容量上界**（`NaniteRaster` 的占位索引缓冲按它钳制）
///
/// 【为什么需要占位索引缓冲】本任务的绘制端仍是"数次数"的占位通道（真实软光栅是任务 18）：
///   `DrawIndexedIndirectCount` 会按命令里的 `firstIndex/indexCount` 去 **绑定索引缓冲**取索引
///   ⇒ 绑定缓冲必须覆盖整个索引段的位置空间，否则是越界读取（本设备**未**启用
///   `robustBufferAccess`，越界不是定义行为）。
/// 【上界怎么来的】簇是索引段的一个连续三角形区间 ⇒ 任意簇的
///   `firstIndex + indexCount ≤ 索引段总索引数 ≤ 簇数上限 × 每簇三角形上限 × 3`。
inline constexpr u32 kNanitePlaceholderIndexCountMax =
    kNaniteMaxBVHClusters * kNaniteMaxClusterTriangles * kNaniteIndicesPerTriangle;
static_assert(kNanitePlaceholderIndexCountMax == 16384u * 64u * 3u,
              "占位索引缓冲的上界必须是 簇数上限 × 每簇三角形上限 × 3");

/// 由簇记录推出该簇的绘制参数（CPU 打包与 GPU **同一份**定义的唯一产地）
///
/// 【越界防御】`triangleOffset × 3` 与 `triangleCount × 3` 都用 64 位中间量算，避免
///   损坏的资产（极大 `triangleOffset`）在 32 位乘法里回绕成一个小值而"看起来合法"。
[[nodiscard]] inline NaniteClusterDrawRange NaniteMakeClusterDrawRange(
        const NaniteClusterRecord& cluster) {
    const u64 firstIndex = (u64)cluster.triangleOffset * (u64)kNaniteIndicesPerTriangle;
    const u64 indexCount = (u64)cluster.triangleCount * (u64)kNaniteIndicesPerTriangle;
    NaniteClusterDrawRange range;
    // 超过 u32 的输入按 u32 上限钳（后续的 `IsIndirectCommandLegal` 会因此判不合法 ⇒ 不画。
    // 正常资产恒不会触发；这里只保证"不产生回绕后的合法假值"）。
    range.firstIndex   = (firstIndex > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (u32)firstIndex;
    range.indexCount   = (indexCount > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (u32)indexCount;
    range.vertexOffset = cluster.vertexOffset;
    range._pad         = 0u;
    return range;
}

/// 一条可见簇引用 → 一条间接绘制命令（约定见本节头注释）
///
/// 【`firstInstance` = 簇号】它是光栅端从命令里**唯一**能拿到的簇标识（`SV_InstanceID`）：
///   用于"这条命令归属哪个簇"的读回比对（`NaniteIndirectCommandMatchesRange`）。
///   【注意：不能拿它当"绘制唯一键"】可见簇引用是 (实例, 簇) 二元组 ⇒ 同一个簇会被多个实例
///   各引用一次、产生多条**完全相同**的命令；绘制端的"每个绘制恰好一次"计数因此用的是
///   `SV_PrimitiveID`（逐次执行的量），而不是按簇号去重。
[[nodiscard]] inline NaniteIndirectCommand NaniteMakeIndirectCommand(
        const NaniteClusterDrawRange& range, u32 clusterIndex) {
    NaniteIndirectCommand cmd;
    cmd.indexCount    = range.indexCount;
    cmd.instanceCount = 1u;                 // 一个簇 = 一次绘制（不是"一个实例一次画多簇"）
    cmd.firstIndex    = range.firstIndex;
    cmd.vertexOffset  = range.vertexOffset;
    cmd.firstInstance = clusterIndex;       // 簇号约定（与 `firstInstance` 的 Vulkan 语义一致）
    return cmd;
}

/// 一条间接命令是否**自洽合法**（读回时的判据；也是"空转/坏命令"的检查）
///
/// 判据：`instanceCount == 1`、`indexCount` 是 3 的倍数且在 `[3, 每簇上限×3]` 内、
///   `firstInstance`（簇号）落在 `[0, clusterCount)`。哨兵（0xFFFFFFFF）与未写过的槽位必然
///   有一条不满足 ⇒ 未写槽位不会被误判为"有效命令"。
[[nodiscard]] inline bool NaniteIsIndirectCommandLegal(const NaniteIndirectCommand& cmd,
                                                       u32 clusterCount) {
    if (cmd.instanceCount != 1u) return false;
    if (cmd.indexCount < kNaniteIndicesPerTriangle) return false;
    if (cmd.indexCount % kNaniteIndicesPerTriangle != 0u) return false;
    const u32 maxIndexCount = kNaniteMaxClusterTriangles * kNaniteIndicesPerTriangle;
    if (cmd.indexCount > maxIndexCount) return false;
    if (cmd.firstInstance >= clusterCount) return false;
    return true;
}

/// 一条命令是否与"该簇应有的绘制参数"**逐项一致**（dump 帧 CPU/GPU 对照的判据）
[[nodiscard]] inline bool NaniteIndirectCommandMatchesRange(
        const NaniteIndirectCommand& cmd, const NaniteClusterDrawRange& range,
        u32 clusterIndex) {
    return cmd.indexCount    == range.indexCount
        && cmd.instanceCount == 1u
        && cmd.firstIndex    == range.firstIndex
        && cmd.vertexOffset  == range.vertexOffset
        && cmd.firstInstance == clusterIndex;
}

/// **CPU 参考的间接命令打包**（§14.8 任务 16 的验收基准；与 GPU 的写入口径逐条同构）
///
/// 输入：
///   · `refs` / `refCount` —— 可见簇引用（`NaniteVisibleClusterRef`；顺序任意，
///     与 GPU 的原子槽位顺序无关 —— GPU 侧的槽位顺序不定，故比较口径是"按 `firstInstance`
///     索引后再逐字段比"，不是按槽位下标逐项比）；
///   · `ranges` / `rangeCount` —— 每簇绘制参数表（`NaniteMakeClusterDrawRange` 的产物）；
///   · `out` / `outCapacity` —— 输出命令（只写容量内）；
///   · `outTruncated`（可空）—— 因容量不足而**未写**的命令条数。
/// 输出：返回值 = 实际写入的命令条数（≤ `outCapacity`）。
///
/// 【与 GPU 侧逐条对应（`Nanite_ClusterBVH.comp.slang` 的接受分支）】
///   ① `ref.cluster >= rangeCount` ⇒ 跳过（GPU 侧对应的防御是 `cluster >= clusterCount`）；
///   ② 只写 `slot < 容量` 的槽位，**计数照常累加**（GPU 的原子计数不受容量影响）；
///   ③ 越界/空指针/0 容量一律返回 0（不崩、不越界写）。
[[nodiscard]] inline u32 NanitePackVisibleIndirectCommands(
        const NaniteVisibleClusterRef* refs, u32 refCount,
        const NaniteClusterDrawRange* ranges, u32 rangeCount,
        NaniteIndirectCommand* out, u32 outCapacity, u32* outTruncated) {
    if (outTruncated != nullptr) *outTruncated = 0u;
    if (refs == nullptr || ranges == nullptr || out == nullptr || outCapacity == 0u) return 0u;

    u32 written = 0u;
    for (u32 i = 0u; i < refCount; ++i) {
        const u32 cluster = refs[i].cluster;
        if (cluster >= rangeCount) continue;                 // ① 越界簇下标防御
        if (written >= outCapacity) {                        // ② 容量截断（计数照常）
            if (outTruncated != nullptr) ++(*outTruncated);
            continue;
        }
        out[written] = NaniteMakeIndirectCommand(ranges[cluster], cluster);
        ++written;
    }
    return written;
}

// ============================================================
// §14.8 任务 18：软光栅（两趟"原子深度键 + 等值复检"写 GBuffer）
//
// 【为什么是两趟而不是"一趟 + interlock"（设计 §5.2 的原文）】
//   设计 §5.2 写的是"每个 cluster 一个 wave，interlock 写 GBuffer"，即用 Slang/HLSL 的
//   `RasterizerOrderedTexture2D`（ROV）拿逐像素互锁。**本仓库的 Slang 版本做不到**：
//   Slang 2026.13 对 compute 入口里的 ROV **静默降级**成普通 `RWTexture2D`（exit 0、
//   `-warnings-as-errors all` 下零诊断、SPIR-V 里没有任何 `OpBeginInvocationInterlockEXT`），
//   而且 SPIR-V 规定 interlock 的 execution mode 只对 Fragment 入口合法（手工汇编后
//   `spirv-val` 报 "Execution mode can only be used with the Fragment execution model."）。
//   ⇒ 单趟写法（"原子最小深度后紧接着写 GBuffer"）**有竞态**：更近的三角形赢了深度，
//   但更远的那个三角形的**颜色写入可能后落地**，像素最终留错属性。
//   ⇒ 本实现：**第 1 趟**只做 `InterlockedMin(深度键)`（R32_UINT 存储图像上的无符号原子最小，
//      **零额外设备特性**）；**第 2 趟**重跑同样的光栅化、对自己的键做**等值复检**，
//      相等才写 GBuffer ⇒ 每个像素恰好一个三角形写一次，多张颜色目标天然一致。
// ============================================================

/// 软光栅两趟共用的 push constant（96B）
///
/// 【与 Slang 的契约】`Engine/Shader/Shaders/Nanite/Nanite_SoftRasterCommon.slang` 的
///   `[[vk::push_constant]] cbuffer NaniteSoftRasterParams` 必须与本结构**逐字段一致**
///   （下面的 static_assert 把 C++ 侧的尺寸/偏移钉住）。
/// 【为什么 view-proj 拆成 4 个"行"】与任务 15 同一理由：Slang 的矩阵行/列主序依赖编译选项，
///   拆成 4 个 float4 + 显式点积后，CPU 与 GPU 乘的是同一个表达式。
/// 【为什么 `depthKeyEpsilon` 留着】两趟的等值复检实测**严格逐位相等**即可（两趟跑的是同一段
///   浮点表达式）；字段留着是为了将来"改精度/改布局"时有一个显式的调节点，默认 0。
struct NaniteSoftRasterParams {
    float vpRows[16];       ///< 偏移 0 ：view-proj 的 4 个行（row-major：`vpRows[r*4+c] = viewProj[c][r]`）
    u32   screenWidth;      ///< 偏移 64：帧缓冲宽（像素）
    u32   screenHeight;     ///< 偏移 68：帧缓冲高（像素）
    u32   maxTriangles;     ///< 偏移 72：只对 `triangleCount ≤ 该值` 的簇走软光栅（§5.2 的 16）
    u32   instanceCount;    ///< 偏移 76：实例域上界（越界引用直接跳过，不读实例表）
    float meshMaxExtent;    ///< 偏移 80：位置量化尺度（整网格最大轴长，§14.19 硬约束①）
    float depthKeyEpsilon;  ///< 偏移 84：等值复检容差（0 = 严格逐位相等）
    u32   materialCount;    ///< 偏移 88：【任务 19】资产材质段条数（0 ⇒ 退化为中性常数并计数）
    u32   _pad1;            ///< 偏移 92
};

static_assert(sizeof(NaniteSoftRasterParams) == 96u,
              "软光栅 push constant 必须 96B（4×float4 + 3×u32 + 3×float/u32）");
static_assert(offsetof(NaniteSoftRasterParams, vpRows)        == 0,  "vpRows 在偏移 0");
static_assert(offsetof(NaniteSoftRasterParams, screenWidth)   == 64, "screenWidth 在偏移 64");
static_assert(offsetof(NaniteSoftRasterParams, screenHeight)  == 68, "screenHeight 在偏移 68");
static_assert(offsetof(NaniteSoftRasterParams, maxTriangles)  == 72, "maxTriangles 在偏移 72");
static_assert(offsetof(NaniteSoftRasterParams, instanceCount) == 76, "instanceCount 在偏移 76");
static_assert(offsetof(NaniteSoftRasterParams, meshMaxExtent) == 80, "meshMaxExtent 在偏移 80");
static_assert(offsetof(NaniteSoftRasterParams, materialCount) == 88, "materialCount 在偏移 88");

/// 【P0 修复（§14.30）】深度解析通道的 push constant（8B）
///
/// 【为什么需要它（这条是本模块最隐蔽的一个真 bug）】深度键是 `RWStructuredBuffer<uint>`
///   （一维、长度 = 宽×高），而**结构化缓冲的 `GetDimensions` 返回的是"元素个数 + 1"**，
///   不是二维宽高 —— 即 `width = 宽×高`、`height = 1`。`Nanite_DepthResolve.frag.slang` 过去
///   用 `u_DepthKey.GetDimensions(width, height)` 当二维尺寸用，于是
///     `if (pixel.y >= height) { depth = 1.0; return; }`
///   对**除第 0 行以外的所有像素**都命中 ⇒ 深度解析只写了第 0 行，其余像素被写成远平面。
///   实测症状：模块接管时 Hi-Z 金字塔恒为 1.0（`hiz_half=[1.000000,1.000000]`、
///   `occluded` 与深度无关），把解析通道的输出强制成常量 0.5 也**不改变**读数 ——
///   因为那行常量写在第 0 行之后、且大多数像素早已在早退里返回 1.0。
///   修法：屏幕尺寸由 CPU 用**显式 push constant** 传进来（不再从缓冲反推二维形状）。
struct NaniteDepthResolveParams {
    u32 screenWidth;    ///< 偏移 0：帧缓冲宽（像素）
    u32 screenHeight;   ///< 偏移 4：帧缓冲高（像素）
};
static_assert(sizeof(NaniteDepthResolveParams) == 8u, "深度解析 push constant 必须 8B");
static_assert(offsetof(NaniteDepthResolveParams, screenHeight) == 4, "screenHeight 在偏移 4");

/// 软光栅读数槽位（扁平 u32；与 `Nanite_SoftRasterCommon.slang` 的 `kSoftStat*` 一一对应）
inline constexpr u32 kNaniteSoftStatRasterClusters  = 0u;   ///< 真正走软光栅的簇数（≤ maxTriangles）
inline constexpr u32 kNaniteSoftStatSkippedClusters = 1u;   ///< 因三角形数超阈值跳过的簇数（任务 22 的活）
inline constexpr u32 kNaniteSoftStatTriangles       = 2u;   ///< 参与光栅化的非退化三角形数（第 1 趟）
inline constexpr u32 kNaniteSoftStatDegenerate      = 3u;   ///< 被丢弃的三角形数（相机后/退化/越界）
inline constexpr u32 kNaniteSoftStatPixels          = 4u;   ///< 通过深度复检、真正写进 GBuffer 的像素数
inline constexpr u32 kNaniteSoftStatNeutralPixels   = 5u;   ///< 【任务 19 起恒 0】用中性常数写入的像素数
/// 【任务 19】用**资产材质段**的真实材质写入的像素数（目标：== `kNaniteSoftStatPixels`）
inline constexpr u32 kNaniteSoftStatMaterialPixels  = 12u;
/// 【任务 19】材质段越界/缺失而退化为中性常数的像素数（正常必须 0；`neutral_material_pixels` 的替代口径）
inline constexpr u32 kNaniteSoftStatFallbackPixels  = 13u;
/// 【P0 修复（§14.30）】深度解析通道**真正写入非远平面深度**的像素数（原子计数，真实 GPU 读回）。
/// 【为什么它取代了旧的 `depth_written` 常量】旧读数在 C++ 侧硬编码为恒 1（"深度解析恒执行"），
///   于是"深度解析其实一列都没写进去"这个真 bug 被一个恒真读数掩盖了一整个任务。
/// 【语义】全屏片元对每个像素只访问一次 ⇒ 它是**去重后的像素数**，与 `kNaniteSoftStatPixels`
///   （通过等值复检的"簇×三角形×像素"写次数，会因键完全相同的平局而更大）不是同一个量。
///   可核对的不变式：本项 ≤ `kNaniteSoftStatPixels` ≤ `kNaniteSoftStatCoveredPixels`。
inline constexpr u32 kNaniteSoftStatDepthResolvedPixels = 14u;
inline constexpr u32 kNaniteSoftStatsCapacity       = 16u;  ///< 读数缓冲条数（与 shader 一致）

/// "该像素没有几何"的深度键哨兵（第 1 趟之前由模块把整张深度键清成它）
inline constexpr u32 kNaniteSoftRasterNoGeometryKey = 0xFFFFFFFFu;

/// 深度键编码（与 `Nanite_SoftRasterCommon.slang` 的 `softRasterDepthKey` **逐字一致**）
///
/// 高 24 位 = NDC 深度（Vulkan `[0,1]`，近 = 0）的**浮点位模式**：非负浮点的位模式与无符号整数
/// 同序 ⇒ `InterlockedMin` 就是"取最近"（与 Lumen 的 `InterlockedMin(asuint())` 同一惯用法）；
/// 低 8 位 = 簇内三角形下标（0..63）⇒ 深度恰好相同时也有全序，第 2 趟的等值复检因此确定。
[[nodiscard]] inline u32 NaniteSoftRasterDepthKey(float ndcZ, u32 triLocal) {
    static_assert(sizeof(float) == sizeof(u32), "float 必须是 32 位");
    // 用 memcpy 取位模式（C++17 没有 bit_cast；reinterpret_cast 在 constexpr 里不可用，
    // 而本函数只用于 CPU 参考/单测，不需要常量求值）
    u32 bits = 0u;
    std::memcpy(&bits, &ndcZ, sizeof(bits));
    return (bits & 0xFFFFFF00u) | (triLocal & 0xFFu);
}

} // namespace he::render
