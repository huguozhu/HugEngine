#pragma once

// ============================================================
// Nanite/NaniteCull.h — 实例剔除 + cluster BVH 剔除 + Hi-Z 遮挡
//
// 【§14.8 任务 3：模块自持的「计数 → 间接绘制」链】
//   任务 1/2 只有生命周期桩；任务 3 起本类自持**四个缓冲**并把"计数"端做出来：
//     · 假簇输入缓冲      —— N 条 `NaniteFakeCluster`（1 个实例、N 个簇）
//     · 间接命令缓冲      —— N 条 `NaniteIndirectCommand`（GPU 压缩写入）
//     · 计数缓冲          —— 单个 u32（GPU 原子累加"实际写入的命令条数"）
//     · 光栅化簇计数缓冲  —— 单个 u32（绘制端每光栅化一个簇原子加一）
//   compute 写命令与计数后插入一次 `ComputeShader → DrawIndirect` 屏障，
//   绘制端（`NaniteRaster`）用 `DrawIndexedIndirectCount` 直接消费该计数。
//   **不改 `GPUCulling`**：模块自持整条链（§14.5 末段）。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法 —— 本类将来接收 Hi-Z 纹理句柄即可，
//   不去 include `GPUCulling.h` 的私有成员）；不得依赖 `MeshBatcher` 的运行时状态
//   —— 它只当**一次性输入**。
//
// 【§14.8 任务 13：实例剔除通道】在原有四条链路之外，本类再自持一组"实例剔除"资源：
//     · 实例缓冲        —— N 条 128B 的 `NaniteInstanceGpuObject`（= GPUSceneObject 契约）
//     · 包围球缓冲      —— N 条 16B 的 `NaniteInstanceSphere`（CPU 从 128B 的 bounds 推一次）
//     · 可见实例列表    —— N 个 u32（GPU 原子压缩写入；CPU 参考剔除给出升序基准）
//     · 可见实例计数    —— 单个 u32（GPU 原子累加；**每帧在命令缓冲内清 0**，见 `RecordInstanceCullPass`）
//     · 计数清零源      —— 单个 u32 常驻 0（TransferSrc；上面那次"清 0"的拷贝源）
//   与任务 3 的假簇链**互相独立**（各自一套缓冲/描述符/PSO），互不影响既有验收读数。
//   【列表不需要逐帧重置】读回只取 `[0, 计数)`，而这些槽位必定由**同一次派发**写入（计数与列表
//   写在同一个着色器里）；不重置反而消掉了"主机 memset 与派发竞争"的隐患。
//
// 【§14.8 任务 14：per-instance cluster BVH 遍历】本类再自持一组"簇 BVH"资源：
//     · 节点缓冲        —— `NaniteBVHNode`（32B/条；CPU 构建器产出，一次性上传）
//     · 叶子簇表缓冲    —— u32（叶子用 [left, left+count)）
//     · 簇球缓冲        —— `NaniteClusterSphere`（16B/条；CPU 从簇记录逐位搬运）
//     · 可见簇列表      —— `NaniteVisibleClusterRef`（8B/条；GPU 原子压缩写）
//     · 可见簇计数      —— 单个 u32（GPU 原子累加）
//     · 已访问节点计数  —— 单个 u32（GPU 原子累加；验收的"遍历访问数"）
//     · 清零源          —— 8B 常驻 0（TransferSrc；上面两个计数每帧的"清 0"拷贝源）
//   两个计数**每帧在命令缓冲内用 4B 拷贝清 0**（照任务 13 修法，不用主机写：见
//   `RecordInstanceCullPass` 里那段负向验证）；可见簇列表**不逐帧重置**，理由与任务 13 相同。
//
//   【Phase 1 → Phase 2 的接线（任务 15 已落地；任务 14 的缺口在此关闭）】
//   Phase 1（`Nanite_InstanceCull`）每帧写两张东西：可见实例**压缩列表**+计数（任务 13 的验收
//   读数）与**可见实例掩码**（`mask[i] = 1/0`，按实例下标寻址）。Phase 2（本类的 BVH 遍历）只处理
//   `mask[i] != 0` 的实例。
//   【为什么用掩码而不是直接消费压缩列表】掩码与遍历的实例域 `[0, min(实例数, 64))` 天然对齐
//   ⇒ 钳制后的子集是**确定的**；压缩列表的槽位顺序由 GPU 原子决定，"取前 64 个"在可见数超过
//   上限时是一个**不确定**的子集，会让 CPU/GPU 的逐项比较失去意义。
//   【顺序怎么保证（**不靠注册顺序**）】两个派发被录制在**同一个帧图 pass** 体内
//   （`RecordCullChainPass`：Phase 1 → 屏障 → Hi-Z 构建 → Phase 2/3），顺序由命令缓冲里的
//   `PipelineBarrier` 显式给出；帧图无法表达这条顺序（两个 pass 都不声明帧图资源 ⇒
//   `RenderGraph::TopologicalSort` 对 inDegree=0 的 pass 按 LIFO 处理，注册顺序 ≠ 执行顺序）。
//   另外下游只读 `[0, 计数)` / 掩码，属于"自洽计数"的第二重保险：任何时刻读到的
//   (计数, 列表) 对都来自同一次派发（上一帧完整 / 清零后的 0 / 本帧完整），不会撕裂。
//
// 【§14.8 任务 15：Phase 2 的 Hi-Z 遮挡 + Phase 3 的 LOD 选择】本类再新增四个自持缓冲：
//     · 可见实例掩码    —— u32/实例（Phase 1 每线程写 0/1，**无需清零**：每个实例都被显式写过）
//     · 每簇 LOD 元数据 —— `NaniteClusterLODInfo`（16B/条；CPU 构建器产出，一次性上传）
//     · 三阶段参数      —— `NaniteCullChainParams`（112B；vp 的 4 行 + 相机 + LOD/Hi-Z 参数）
//     · 三阶段读数      —— 扁平 u32（通过视锥/遮挡/级直方图/访问节点数），每帧命令缓冲内清零
//   Hi-Z 金字塔**复用既有资源与口径**（`GPUCulling` 的那张 `R32_FLOAT` / ≤8 层纹理 + 与其
//   `HiZDownsample.comp.slang` 逐字相同的"2×2 取最小深度"公式），但**构建由本类自己做**
//   （`BuildHiZPyramid`，逐目标 mip 一个专属描述符集）—— 既有 `GPUCulling::BuildHiZPyramid`
//   在本引擎里构建不出正确金字塔（逐 mip 更新同一个描述符集 vs GPU 执行期读描述符），
//   完整证据与上游修法见 `NaniteRenderer.h` 的 `NaniteHiZSource` 注释。
//   【金字塔的三条约定（核实自既有实现，见 `NaniteTypes.h` 的任务 15 小节）】
//     ① 格式 `R32_FLOAT`、最多 8 层；② 层 L 存 2^L×2^L 足迹的**最小深度**（近 = 小）；
//     ③ **mip0 从未被写入** ⇒ 采样层下限钳到 1（`kNaniteHiZMinMip`）。
//
// 【§14.8 任务 16：可见簇列表 → 间接绘制参数（把 `u_VisibleClusters` 真正接到光栅端）】
//   任务 14/15 把可见簇算出来了却没有消费者（光栅端当时消费的是任务 3 的假簇链）——本任务补上：
//     · 每簇绘制参数表 —— `NaniteClusterDrawRange`（16B/条；`SetClusterBVH` 一次性上传），
//       由 `NaniteMakeClusterDrawRange` 从 `.nanite` 簇记录逐位搬运；
//     · 间接命令缓冲   —— `NaniteIndirectCommand`（20B/条 = `VkDrawIndexedIndirectCommand`），
//       与可见簇引用**同一个原子槽位**写入（容量也刻意相同 ⇒ 槽位一一对应）；
//     · 绘制计数       —— 单个 u32，只在"真的写了命令"时 +1 ⇒ 恒 ≤ 容量 ⇒ 恒 ≤ maxDrawCount。
//   光栅端（`NaniteRaster::RecordRasterPass`）用 `DrawIndexedIndirectCount` 消费它们。
//   【无空转的三重保证】① 计数与命令同一次派发写出；② 只画 `[0, count)`；③ 计数每帧在命令缓冲内
//   清 0（常驻 0 源 + 4B 拷贝；主机写清零会与派发竞争，任务 13 实测错读成两倍）。
//   【绘制计数为什么不复用可见簇计数】后者是任务 14/15 的验收读数，必须**不截断**；而绘制计数
//   必须满足"≤ 间接缓冲容量"这条硬约束。两者分开后，容量足够时相等，容量不足时可见计数保真、
//   绘制计数被钳住并把截断条数记进 `kNaniteCullStatDrawTruncated`。
// ============================================================

#include "Nanite/NaniteTypes.h"
#include "Nanite/NaniteUpload.h"   // 【任务 14】`NaniteClusterBVH`（构建产物；RHI-free 头）
#include "RHI/RHI.h"
#include "Math/Math.h"   // 【任务 13】float3 / float4x4（相机视锥与合成实例网格的输入类型）

#include <functional>   // 【任务 15】`NaniteRenderer` 的 Hi-Z 源回调（纹理/深度）经这里传递
// 【顺手修掉的一处既有笔误】本行过去是 `#include <memory>#include <vector>`（两条指令挤在同一
//   行 ⇒ MSVC 只处理第一条并给出 C4067，`<vector>` 从未真正被包含，一直靠传递包含碰巧编过）。
//   本任务起本头文件新增的成员里仍有 `std::vector`，故把它拆成两行（零行为变化）。
#include <memory>
#include <vector>
#include <span>   // 【任务 14】SetClusterBVH 的簇记录视图

namespace he::render {

/// Nanite_Cull.comp.slang 的 push constant（逐字段对应；static_assert 钉住 8 字节）
struct alignas(4) NaniteCullParams {
    u32 clusterCount;           // 本帧假簇数量
    u32 vertexCountPerCluster;  // 每条间接命令的 indexCount（假数据 = 3）
};
static_assert(sizeof(NaniteCullParams) == 8, "NaniteCullParams 必须与 Slang cbuffer 一致（2×u32）");

/// 【§14.8 任务 13】Nanite_InstanceCull.comp.slang 的 push constant
///
/// 【为什么用 push constant 传视锥】6 个平面每帧只变一次（相机），且远小于 128B 的下限；
///   走小块 push constant 不必再建 UBO/描述符，也不会与既有 pass 的描述符集打架。
/// 【布局】`planes[6]`（std430 下 float4 步长 16 ⇒ 96B）+ 4 个 u32（16B）= 112B；
///   与 Slang 侧的 `NaniteInstanceCullParams` 逐字段一致。
struct alignas(16) NaniteInstanceCullParams {
    float planes[6][4];   // 偏移 0：世界空间视锥六平面（[左,右,下,上,近,远]，n 已归一化）
    u32   instanceCount;  // 偏移 96：本帧实例数
    u32   _pad0;          // 偏移 100
    u32   _pad1;          // 偏移 104
    u32   _pad2;          // 偏移 108
};
static_assert(sizeof(NaniteInstanceCullParams) == 112,
              "NaniteInstanceCullParams 必须与 Slang cbuffer 一致（6×float4 + 4×u32 = 112B）");
static_assert(offsetof(NaniteInstanceCullParams, instanceCount) == 96,
              "instanceCount 必须紧跟 6 个 float4（偏移 96）");

/// 【§14.8 任务 14】Nanite_ClusterBVH.comp.slang 的 push constant
///
/// 【布局】`planes[6]`（96B）+ 4 个 u32（16B）= 112B，与任务 13 的实例剔除同形（同一套视锥提取）。
///   `visibleCapacity` 进 push constant 是为了让 shader 自己判断"槽位是否越出可见列表容量"，
///   从而 GPU 的写入与 CPU 参考的写入口径（只写容量内、计数照常累加）逐条一致。
struct alignas(16) NaniteClusterBVHParams {
    float planes[6][4];     // 偏移 0：世界空间视锥六平面（[左,右,下,上,近,远]，n 已归一化）
    u32   instanceCount;    // 偏移 96：本帧实例数（合成实例表的条数）
    u32   clusterCount;     // 偏移 100：簇数（= 簇球表条数）
    u32   nodeCount;        // 偏移 104：BVH 节点数
    u32   visibleCapacity;  // 偏移 108：可见簇列表容量
};
static_assert(sizeof(NaniteClusterBVHParams) == 112,
              "NaniteClusterBVHParams 必须与 Slang cbuffer 一致（6×float4 + 4×u32 = 112B）");
static_assert(offsetof(NaniteClusterBVHParams, instanceCount) == 96,
              "instanceCount 必须紧跟 6 个 float4（偏移 96）");
static_assert(offsetof(NaniteClusterBVHParams, visibleCapacity) == 108,
              "visibleCapacity 必须在偏移 108");

/// 【§14.8 任务 15】三阶段剔除的参数缓冲（112B；GPU 侧是 `Nanite_ClusterBVH.comp.slang` 的
/// `CullChainParams`，逐字段一致）
///
/// 【为什么单独一个 SSBO 而不是塞进 push constant】push constant 限 128B，而视锥六平面已经占了
///   96B（它们是逐节点测试最热的读，放 push constant 最划算）。这里再加 112B 会超限 ⇒ 把
///   "每帧只读一次"的参数（vp 的 4 行、相机、LOD/Hi-Z 标量）放进一个小 SSBO。
/// 【为什么把 viewProj 拆成 4 个"行"】与任务 14 拆 `localToWorld` 同一个理由：Slang 的
///   `float4x4` 行/列主序依赖编译选项，拆成 4 个 float4 + 显式点积后，CPU 与 GPU 乘的表达式同一。
///   本结构里 `vpRows[r*4+c]` 是**行优先**存储（row r），Slang 侧对应 `vpRow0..vpRow3`。
struct alignas(16) NaniteCullChainParams {
    float vpRows[16];     // 偏移 0  ：view-proj 的 4 个行（row 优先；`viewProj[c][r]`）
    float cameraPos[3];   // 偏移 64 ：相机世界坐标（LOD 判据的距离基准）
    float _pad0;          // 偏移 76
    float focalPixels;    // 偏移 80 ：像素焦距（0 ⇒ 关闭 LOD 选择）
    float lodThreshold;   // 偏移 84 ：LOD 阈值（像素；默认 1.0）
    float screenW;        // 偏移 88 ：屏幕宽（Hi-Z 选层的像素换算）
    float screenH;        // 偏移 92 ：屏幕高
    u32   hizMipCount;    // 偏移 96 ：Hi-Z 金字塔层数（< 2 ⇒ 关闭遮挡测试）
    u32   lodEnabled;     // 偏移 100：1 = 打开 LOD 选择（与 focalPixels > 0 同时成立才算）
    u32   _pad1;          // 偏移 104：绘制容量（`misc.z`；任务 16 的截断门）
    u32   hizFlip;        // 偏移 108：【P0】1 = Hi-Z 采样 UV 的 y 翻转（负高度视口的正确约定）
};
static_assert(sizeof(NaniteCullChainParams) == 112,
              "NaniteCullChainParams 必须与 Slang 结构体一致（4×float4 + float4 + float4 + uint4）");
static_assert(offsetof(NaniteCullChainParams, vpRows)      == 0,   "vpRows 必须在偏移 0");
static_assert(offsetof(NaniteCullChainParams, cameraPos)   == 64,  "cameraPos 必须在偏移 64");
static_assert(offsetof(NaniteCullChainParams, focalPixels) == 80,  "focalPixels 必须在偏移 80");
static_assert(offsetof(NaniteCullChainParams, screenW)     == 88,  "screenW 必须在偏移 88");
static_assert(offsetof(NaniteCullChainParams, hizMipCount) == 96,  "hizMipCount 必须在偏移 96");
static_assert(offsetof(NaniteCullChainParams, lodEnabled)  == 100, "lodEnabled 必须在偏移 100");

/// 【§14.8 任务 15】三阶段读数的 GPU 缓冲布局（扁平 u32；与 shader 的 `u_Stats` 槽位一一对应）
///
/// ```text
/// [0]  通过视锥的簇引用数（Phase 2 前半）
/// [1]  被 Hi-Z 判为遮挡的簇引用数（Phase 2 后半）
/// [2..9] "选中级别分布"直方图：lodHistogram[L] = 被选中的 L 级簇引用数（L = 0..7）
/// [10] 访问过的 BVH 节点数（全部实例求和）
/// [11] 【任务 16】因**绘制容量**不足而未写间接命令的簇数（截断计数）
/// [12] 【P0 修复】被遮挡且屏幕包围盒中心落在**上半屏**（ndc.y > 0）的簇数
/// [13] 【P0 修复】被遮挡且屏幕包围盒中心落在**下半屏**（ndc.y <= 0）的簇数
/// [14] 【P0 实验】Hi-Z mip1 上半屏平均深度 ×1000（32×32 网格；+0.5 截断，读回按 `/1000` 报）
/// [15] 【P0 实验】Hi-Z mip1 下半屏平均深度 ×1000
/// ```
///
/// 【为什么要有 [12]/[13] 两个"上半屏/下半屏"槽（P0 修复的可复现判据）】
///   本引擎的离屏通道用**负高度视口**（`GBufferRenderer_CPU.cpp:61` 的 `SetViewport({0,h,w,-h,0,1})`），
///   于是 NDC y=+1 落在帧缓冲第 0 行、纹理 UV 的 v 向下增长 ⇒ `s.y = ndc.y*0.5+0.5` 与纹理行是
///   **镜像**的（正确写法是 `v = 0.5 - 0.5*ndc.y`，同引擎内已由 SSR 的镜面解析对照实测确认，
///   见 `GI/SSR.frag.slang:55-68`）。把"被遮挡的簇落在哪半屏"单独计数，就能用一个**上下不对称的
///   遮挡场景**直接判定镜像：遮挡物只在半屏时，错误约定会把**另一半屏**的簇判成被遮挡。
///   两槽都用**投影包围盒中心的 ndc.y** 分类（与采样用的 UV 约定无关），因此不会自我印证。
inline constexpr u32 kNaniteCullStatOccludedUpper = 12u;
inline constexpr u32 kNaniteCullStatOccludedLower = 13u;
/// 【P0 实验】Hi-Z mip1 的上/下半屏平均深度 ×1000（32×32 采样网格；由 `Nanite_ClusterBVH.comp.slang`
///   的 0 号线程写入）。用途：证明"遮挡物只在半屏"这一实验前提，并把两档 UV 的遮挡差异
///   归因到**真实深度场**，而不是只靠数字变大变小。
inline constexpr u32 kNaniteCullStatHiZUpperMeanMilli = 14u;
inline constexpr u32 kNaniteCullStatHiZLowerMeanMilli = 15u;
///
/// 【为什么平坦 u32 而不是结构体】std430 下"结构体里的 u32 数组"步长细节依赖编译器，
///   平坦数组 + 常量下标没有任何布局歧义（任务 14 的 `uint4 link` 已经踩过这一类坑）。
inline constexpr u32 kNaniteCullStatFrustumPass = 0u;
inline constexpr u32 kNaniteCullStatOccluded    = 1u;
inline constexpr u32 kNaniteCullStatLodBase     = 2u;   ///< 直方图起点（8 个槽）
inline constexpr u32 kNaniteCullStatVisited     = 10u;  ///< 访问节点数
/// 【任务 16】因绘制容量不足被截断的簇数（与 shader 的 `kStatDrawTruncated` 逐条对应）
inline constexpr u32 kNaniteCullStatDrawTruncated = 11u;
inline constexpr u32 kNaniteCullStatsU32        = 16u;  ///< 实际使用的槽数（含 P0 的四个诊断槽）
inline constexpr u32 kNaniteCullStatsCapacity   = 16u;  ///< 缓冲容量（16B 对齐，一次拷贝清零）
inline constexpr u64 kNaniteCullStatsBytes      = sizeof(u32) * kNaniteCullStatsCapacity;
static_assert(kNaniteCullStatsU32 <= kNaniteCullStatsCapacity, "读数槽位不得超出缓冲容量");

/// 【§14.8 任务 15】模块自持的 Hi-Z 金字塔构建所需的**目标层存储视图**数量（= 金字塔最大层数）
///   —— 只写 mip1..mipCount-1，故视图也只按这些层建。
inline constexpr u32 kNaniteHiZBuildMaxViews = kNaniteMaxHiZMips;

/// 【§14.8 任务 15】Hi-Z 下采样（`Nanite_HiZDownsample.comp.slang`）的 push constant（24B）
///
/// 【布局与既有 `HiZDownsample.comp.slang` 的 `Params` 同形】`uint2 srcSize; uint2 dstSize;
///   uint srcMip; uint _pad;` —— 源/目标尺寸都是**像素**，`srcMip == 0` 表示源是本帧深度纹理
///   （用采样器按目标纹素中心取 2×2），`srcMip > 0` 表示源是金字塔的该层（存储图像显式取 2×2）。
struct alignas(4) NaniteHiZDownsampleParams {
    u32 srcW = 0u;     ///< 偏移 0 ：源级宽（像素）
    u32 srcH = 0u;     ///< 偏移 4 ：源级高（像素）
    u32 dstW = 0u;     ///< 偏移 8 ：目标级宽（像素）
    u32 dstH = 0u;     ///< 偏移 12：目标级高（像素）
    u32 srcMip = 0u;   ///< 偏移 16：源层号（0 = 深度纹理）
    u32 _pad = 0u;     ///< 偏移 20：填充到 24B
};
static_assert(sizeof(NaniteHiZDownsampleParams) == 24,
              "Hi-Z 下采样的 push constant 必须与 Slang 侧一致（24B）");
static_assert(offsetof(NaniteHiZDownsampleParams, dstW)   == 8,  "dstW 必须在偏移 8");
static_assert(offsetof(NaniteHiZDownsampleParams, srcMip) == 16, "srcMip 必须在偏移 16");

class NaniteCull {
public:
    NaniteCull() = default;
    ~NaniteCull() = default;

    NaniteCull(const NaniteCull&) = delete;
    NaniteCull& operator=(const NaniteCull&) = delete;

    /// 建立"计数 → 间接绘制"链的自持资源：四个缓冲 + compute PSO + 描述符集。
    /// @param rasterCountBuffer 由本类创建并持有的"已光栅化簇计数缓冲"（绘制端引用它）
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);

    /// 释放自持资源（缓冲/PSO/描述符集布局）
    void Shutdown();
    void OnResize(u32 width, u32 height);

    [[nodiscard]] bool IsReady() const {
        return m_Device != nullptr && m_PSO != nullptr && m_InstanceCullPSO != nullptr
            && m_BVHPSO != nullptr;   // 【任务 14】BVH 遍历的 compute 管线也必须建成
    }

    /// 设置本帧假簇数量（超上限钳制）。由 `NaniteRenderer::AddPasses` 从
    /// `NaniteSettings::fakeClusters` 转发，是任务 3 的唯一输入。
    void SetFakeClusterCount(u32 count);
    [[nodiscard]] u32 GetFakeClusterCount() const { return m_FakeClusterCount; }

    // ============================================================
    // §14.8 任务 13：实例剔除（视锥 → 可见实例列表 + 计数）
    // ============================================================

    /// 设置本帧实例剔除的输入：相机（世界空间视锥）与**合成实例网格**条数。
    ///
    /// 【实例来源与坐标系（如实说明：它们**不是**场景实例）】本任务还没有"场景 → 模块实例表"
    ///   的接入点（属任务 14+），验收要的是"GPU 与 CPU 参考逐项一致"这个**可判定的等价性**，
    ///   因此这里用一张**合成**实例网格：
    ///     · 在 **NDC（view-projection 空间）** 上摆一个 `nx × ny` 网格（整体外扩 1.25 倍 ⇒
    ///       边缘必然落到视锥外），再经 `inverse(viewProj)` **反投影到世界空间**；
    ///     · 另取几个下标**故意**摆到视锥外（NDC 2.5）、**故意跨越**右侧平面（NDC 恰好 1.0 +
    ///       足够的半径）、以及一个 `indexCount = 0` 的空实例（验证"空实例跳过"规则）；
    ///     · 每实例的 128B 条目按 `GPUSceneObject` 契约填：`localToWorld` = 平移、`boundsMin/Max`
    ///       = 球心 ± 半径、`indexCount` 非 0；包围球再由 `NaniteSphereFromInstanceBounds`
    ///       从 `boundsMin/Max` 推出 ⇒ **球确实来自 128B 契约**，而不是另一个来源。
    ///   · 网格随相机走（用 `cameraPosition` 与 `viewProj` 反投影），因此相机移动时样本集合
    ///     依然覆盖"里/外/跨越"三类，不依赖硬编码的世界坐标。
    /// 【调用时机】`NaniteRenderer::AddPasses` 每帧调用一次（帧图构建期）。
    ///
    /// 【§14.8 任务 15 新增的三个入参】`screenW/H` 与 `fovYDegrees` 用来算 Phase 3 的**像素焦距**
    ///   （`focalPixels = 0.5 × screenH / tan(fovY/2)`，见 `NaniteClusterLODFocalPixels`）。
    ///   取"相机 fov + 屏幕高度"而不是"反解 viewProj 的 m11"：view-proj 是 `P×V`，
    ///   它的 (1,1) 元素被视图旋转污染，反解出来的焦距在相机有俯仰/偏航时是错的。
    void SetCullChainFrame(const float4x4& viewProj, const float3& cameraPosition,
                           u32 screenWidth, u32 screenHeight, float fovYDegrees,
                           u32 testInstanceCount);

    /// 录制 `Nanite_InstanceCull` pass：
    ///   ① 上传合成实例表 + 包围球（主机可见缓冲，与任务 3 同款）；
    ///   ② CPU 参考剔除（`NaniteCullInstancesCPU`）并保存结果，供 dump 帧逐项比较；
    ///   ③ **命令缓冲内**把可见计数清 0（4B 拷贝，GPU 有序）+ `Transfer → Compute` 屏障；
    ///   ④ Dispatch（每实例一个线程）+ `Compute → Compute|Transfer` 屏障（后者为下一帧的清零消 WAR）。
    /// 【为什么清零必须进命令缓冲】录制期的主机写会与派发竞争（CPU 领先 GPU ⇒ 两帧原子累加叠加，
    ///   实测读回恰为 CPU 参考的 2 倍）。完整负向验证见 `NaniteCull.cpp` 的 `RecordInstanceCullPass`。
    void RecordInstanceCullPass(rhi::IRHICommandList* cmd);

    // ── 实例剔除的读回访问（模块内部与 `NaniteRenderer::LogCull3Readback` 使用）──
    [[nodiscard]] rhi::IRHIBuffer* GetVisibleInstanceBuffer()      const { return m_VisibleInstanceBuf.get(); }
    [[nodiscard]] rhi::IRHIBuffer* GetVisibleInstanceCountBuffer() const { return m_VisibleInstanceCountBuf.get(); }
    /// 【§14.8 任务 18】本帧的实例表（128B/条 `NaniteInstanceGpuObject` 契约）。
    /// 软光栅按可见簇引用的 `instance` 取它的**平移列**（`localToWorld[12..14]`）把网格空间的
    /// 簇顶点搬到世界空间；表内容由 Phase 1 的 `UploadInstanceCullInputs` 写入。
    [[nodiscard]] rhi::IRHIBuffer* GetInstanceBuffer()             const { return m_InstanceBuf.get(); }
    /// CPU 参考剔除的可见实例下标（升序）—— 最近一次 `RecordInstanceCullPass` 的结果
    [[nodiscard]] const std::vector<u32>& GetCpuVisibleInstances() const { return m_CpuVisibleInstances; }
    /// 本帧合成实例条数（= 最近一次 `SetInstanceCullFrame` 的钳制后入参）
    [[nodiscard]] u32 GetTestInstanceCount() const { return m_TestInstanceCount; }
    [[nodiscard]] static constexpr u32 MaxTestInstances() { return kNaniteMaxTestInstances; }

    // ============================================================
    // §14.8 任务 14：per-instance cluster BVH（构建产物入库 + 每帧深度优先遍历）
    // ============================================================

    /// 按 `.nanite` 的簇记录构建 BVH 并**一次性上传**四个只读缓冲（节点 / 叶子簇表 / 簇球 /
    /// LOD 元数据）。
    ///
    /// 【调用时机与次数】`NaniteRenderer::EnsureAssetUploaded` 在开关开启时**只调一次**
    ///   （资产构建成功后）。之后每帧不再碰这四个缓冲。
    /// 【簇数上限】按 `kNaniteMaxBVHClusters` 截断（超出时打印一次中文告警，不静默）；
    ///   被截断掉的是"下标 ≥ 上限"的簇 —— 可见簇引用表的大小由这个上限推出。
    /// 【§14.8 任务 15 新增的 `lodOffsets`】`.nanite` 的 LOD 段（"该级第一个出现簇的下标"）。
    ///   它只用于 `BuildNaniteClusterLODInfo`（Phase 3 的级直方图与根簇判定）；传空表示
    ///   "只有一级"，元数据仍会生成（全部记为 0 级）。
    /// 【失败】设备/PSO 未就绪、构建失败 ⇒ 返回 false（此后该 pass 直接跳过，不派发）。
    /// 【同步约定】本函数只在**一次性启动路径**上被调用（与任务 12 的资产上传同一时机），
    ///   此缓冲尚未被任何已提交的 GPU 工作引用 ⇒ 主机 `Map` 写入不存在竞争。
    [[nodiscard]] bool SetClusterBVH(std::span<const NaniteClusterRecord> clusters,
                                     std::span<const u32>                 lodOffsets);

    /// 本 pass 是否可用（BVH 已入库 + PSO/描述符集就绪）
    [[nodiscard]] bool IsClusterBVHReady() const { return m_BVHReady; }

    /// 【§14.8 任务 15】把 Phase 1 与 Phase 2/3 录制进**同一个命令缓冲**（同一个帧图 pass 体内）：
    ///   ① Phase 1：`RecordInstanceCullPass`（清零 → 上传 → 派发 → 屏障；含可见性掩码）；
    ///   ② Hi-Z 金字塔：先做整图布局转换（可写）→ `BuildHiZPyramid`（逐目标 mip 专属描述符集，
    ///      复用既有纹理与"2×2 取最小深度"口径）→ 再转回可采样（同时是采样前的内存屏障）；
    ///   ③ Phase 2/3：**命令缓冲内**清零三阶段读数 → 派发 BVH 遍历（Phase 1 掩码 → 视锥 → Hi-Z
    ///      → LOD 选择）→ 屏障。
    /// 【顺序为什么可靠】三段都在**一个** pass 体内、靠命令缓冲里的屏障定序 —— 不依赖帧图的两个
    ///   pass "恰好按注册顺序执行"（那是不成立的：`RenderGraph::TopologicalSort` 对 inDegree=0 的
    ///   pass 按 LIFO 处理）。帧图层面本 pass 只声明 `reads = {gbDepth}`（它真的读：Hi-Z 由本帧
    ///   深度下采样而来），因此它被排在 `GB_Clear` 之后（本帧深度已经画完）。
    /// @param hizTexture 本帧的 Hi-Z 纹理（空 ⇒ 退化为模块自建的 1×1 占位纹理，遮挡关闭）
    /// @param depthTexture 本帧的深度纹理（金字塔的输入；空 ⇒ 不构建金字塔）
    /// @param enableOcclusion cfg 键 `nanite_hiz`（false ⇒ 不构建金字塔、层数传 0、恒不遮挡）
    /// @param hizFlipY 【P0 修复】Hi-Z 采样 UV 的 y 是否翻转（负高度视口的正确约定）。
    ///   由 `NaniteSettings::hizFlip` 转发；`params.misc.w` 把它带进 shader。
    ///   `false` 只用于"历史镜像约定"的可复现 A/B 对照（cfg 键 `nanite_hiz_flip=0`）。
    void RecordCullChainPass(rhi::IRHICommandList* cmd,
                             rhi::IRHITexture* hizTexture, rhi::IRHITexture* depthTexture,
                             bool enableOcclusion, bool hizFlipY);

    /// 【§14.8 任务 15】CPU 参考三阶段剔除（dump 帧算一次；输入与 GPU **同一份比特**：同一个视锥、
    ///   同一张 128B 实例表、同一棵 BVH、同一张簇球表、同一张 LOD 元数据表、同一个实例域钳制、
    ///   同一个像素焦距与阈值）。
    ///   【Hi-Z 在 CPU 侧恒为关闭】CPU 拿不到金字塔的逐 texel 内容（RHI 的 `CopyTextureToBuffer`
    ///   只读 mip0，而本引擎的金字塔从不写 mip0）⇒ 参考实现传空采样器（= "Hi-Z 关闭"口径）。
    ///   这也是验收口径允许的：Hi-Z 打开档允许差异，但必须可解释（`LogCull3Readback`
    ///   会打印"GPU 独有的簇数（必须 0）/ CPU 独有的簇数（= 被遮挡剔除数）"与选层分布）。
    /// @param outVisible 输出可见簇引用（会被 resize 到可见数）
    /// @return 三阶段读数（Phase 1 掩码 → 视锥 → LOD；`visited/frustum/occluded/lodHistogram`）
    NaniteClusterBVHTraversalStats RunCullChainCPUReference(
        std::vector<NaniteVisibleClusterRef>& outVisible) const;

    /// 【§14.8 任务 15】按既有 Hi-Z 口径构建金字塔（写 mip1..mipCount-1），
    /// 并在构建前后各做一次**整图**布局转换（GENERAL ↔ 只读）。
    /// 【为什么要模块自建】见 `NaniteRenderer.h` 的 `NaniteHiZSource` 注释（既有
    ///   `GPUCulling::BuildHiZPyramid` 逐 mip 更新同一描述符集 ⇒ 实测全 0）。
    /// 【正确性靠什么】每个目标 mip 一个**专属描述符集**（各绑定每帧只写一次），
    ///   因此不存在"最后一次主机写对整段命令缓冲生效"的次序依赖。
    /// @param pyramid 目标金字塔纹理（GPUCulling 的 R32_FLOAT、≤8 层纹理；模块只借用不持有）
    /// @param depth   本帧深度纹理（金字塔第 1 级的输入）
    /// @return 真正构建出的层数（< 2 ⇒ 调用方应把遮挡测试关掉）
    u32 BuildHiZPyramid(rhi::IRHICommandList* cmd, rhi::IRHITexture* pyramid,
                        rhi::IRHITexture* depth, u32 screenW, u32 screenH);

    // ── 任务 14/15 的读回访问（`NaniteRenderer::LogCull3Readback` 使用）──
    [[nodiscard]] rhi::IRHIBuffer* GetVisibleClusterBuffer()      const { return m_VisibleClusterBuf.get(); }
    [[nodiscard]] rhi::IRHIBuffer* GetVisibleClusterCountBuffer() const { return m_VisibleClusterCountBuf.get(); }
    /// 【任务 15】三阶段读数缓冲（扁平 u32，槽位见 `kNaniteCullStat*`）
    [[nodiscard]] rhi::IRHIBuffer* GetCullStatsBuffer()           const { return m_CullStatsBuf.get(); }
    /// 【任务 15】可见实例掩码缓冲（`mask[i] != 0` ⇒ Phase 1 判可见；调试/对照用）
    [[nodiscard]] rhi::IRHIBuffer* GetVisibleMaskBuffer()         const { return m_VisibleMaskBuf.get(); }
    /// 【任务 15】每簇 LOD 元数据（CPU 侧镜像；数量 == 参与剔除的簇数）
    [[nodiscard]] const std::vector<NaniteClusterLODInfo>& GetClusterLODInfo() const { return m_LODInfo; }

    // ============================================================
    // 【§14.8 任务 26】调试可视化的两个只读数据源 + 每簇 BVH 深度镜像
    //
    // 【为什么由本类提供】这三份数据都是**剔除段**的资产：簇球表与 LOD 元数据是 `SetClusterBVH`
    //   一次性上传的缓冲，BVH 深度只有持有 CPU 镜像的那棵树才推得出来（把 CPU 镜像复制一份到
    //   渲染端等于让"GPU 与 CPU 看的是同一棵树"这条不变量多一个分叉点）。
    //   可视化只是**读者**：它不改任何剔除状态、不在每帧路径上加一次计算。
    // ============================================================

    /// 簇包围球表（16B/条，**按资产簇下标**索引；与 GPU 遍历读的是同一份比特）
    [[nodiscard]] rhi::IRHIBuffer* GetBVHSphereBuffer() const { return m_BVHSphereBuf.get(); }
    /// 每簇 LOD 元数据（16B/条，`lodLevel` 是可视化模式 3 的唯一输入）
    [[nodiscard]] rhi::IRHIBuffer* GetLODInfoBuffer() const { return m_LODInfoBuf.get(); }

    /// 【§14.8 任务 26】每簇的 BVH 节点深度（根 = 1；**懒计算一次**，只为调试可视化服务）
    ///
    /// 【懒计算的边界（这是"默认关零影响"的一部分）】本函数只在可视化档位开启时被调用
    ///   （`NaniteRenderer::EnsureDebugViewReady` 门控），第一次调用走一次 O(节点数 + 簇数) 的
    ///   显式栈遍历，此后返回同一份缓存。默认档一个字节都不算、不分配。
    /// 【长度】恒 == 参与 BVH 的簇数（`GetBVHClusterCount()`）；BVH 未入库时返回空 span。
    [[nodiscard]] std::span<const u32> EnsureClusterBVHDepths();

    // ── 【§14.8 任务 16】可见簇 → 间接绘制参数的读回访问（`NaniteRenderer` 与光栅端使用）──
    /// 间接绘制命令缓冲（20B/条；与可见簇引用同槽位、同容量）
    [[nodiscard]] rhi::IRHIBuffer* GetIndirectDrawBuffer()  const { return m_IndirectDrawBuf.get(); }
    /// 绘制计数缓冲（单个 u32；`DrawIndexedIndirectCount` 的 countBuffer）
    [[nodiscard]] rhi::IRHIBuffer* GetDrawCountBuffer()     const { return m_DrawCountBuf.get(); }
    /// 间接命令缓冲的容量上界（= `maxDrawCount`）
    [[nodiscard]] u32 GetMaxIndirectDraws() const { return kNaniteMaxIndirectDraws; }
    /// 每簇绘制参数表（CPU 侧镜像；CPU 参考打包与 GPU 读的是**同一份比特**）
    [[nodiscard]] const std::vector<NaniteClusterDrawRange>& GetClusterDrawRanges() const {
        return m_ClusterDrawRanges;
    }
    /// 【任务 16】设置本帧的绘制容量（cfg 键 `nanite_draw_capacity`；0 = 用容量上界）
    ///
    /// 钳制口径与 GPU 完全一致：`min(入参, kNaniteMaxIndirectDraws)`，0 视为上界。
    /// 它是"截断路径"的**可复现自证开关**：把它设小，就能在真实 GPU 上看到
    /// `visible` 保持真值、`draws` 被钳住、截断计数非 0，且不越界（`maxDrawCount` 之下）。
    void SetDrawCapacity(u32 capacity) {
        m_FrameDrawCapacity = (capacity == 0u || capacity > kNaniteMaxIndirectDraws)
                            ? kNaniteMaxIndirectDraws : capacity;
    }
    [[nodiscard]] u32 GetFrameDrawCapacity() const { return m_FrameDrawCapacity; }
    /// 【§14.8 任务 15】把"CPU 参考可见、GPU 未见"的簇做一次**选层分布**统计 —— 这是"Hi-Z 打开档
    /// 与关闭档差异"的量化解释：被剔除的簇各自会落在金字塔的哪一层上。
    ///
    /// 【为什么放在这里】世界球 = 实例平移（`m_TestInstances[i].localToWorld[12..14]`）+ 网格空间
    ///   球心，这份数据只有本类持有；渲染层不该再去拼一份实例表。
    /// 【口径】投影用与 shader **同一份** view-proj 行、选层用 `NaniteHiZSelectMip`（同公式）。
    /// @param cpuVisible / gpuVisible 两边都已按 (instance, cluster) 升序
    /// @param outMipHistogram 长度 `kNaniteMaxHiZMips`（调用方清零）
    /// @param outProjectedOffscreen 完全在屏幕外/投影失败（`NaniteProjectSphereToScreen` 返回 false）
    ///        而无法参与 Hi-Z 的个数 —— 正常情况下应当为 0（它们本来也不该被剔除）
    /// @return CPU 独有的簇数（Hi-Z 关闭时应当为 0）
    [[nodiscard]] u32 CountOccludedClustersByMip(
        const std::vector<NaniteVisibleClusterRef>& cpuVisible,
        const std::vector<NaniteVisibleClusterRef>& gpuVisible,
        u32 outMipHistogram[kNaniteMaxHiZMips],
        u32* outProjectedOffscreen) const;
    [[nodiscard]] u32 GetBVHNodeCount()    const { return (u32)m_BVHData.nodes.size(); }
    [[nodiscard]] u32 GetBVHDepth()        const { return m_BVHData.depth; }
    [[nodiscard]] u32 GetBVHClusterCount() const { return m_BVHData.clusterCount; }
    [[nodiscard]] u32 GetBVHInstanceDomain() const { return m_BVHInstanceDomain; }
    [[nodiscard]] u32 GetBVHVisibleCapacity() const { return kNaniteMaxVisibleClusterRefs; }
    [[nodiscard]] static constexpr u32 MaxBVHInstances() { return kNaniteMaxBVHInstances; }

    /// 【§14.8 任务 15】本帧的 LOD/Hi-Z 参数（读回日志用；都是 `SetCullChainFrame` 与
    ///   `RecordCullChainPass` 时记下的真值，不是重新推断的）
    [[nodiscard]] float GetFrameFocalPixels()  const { return m_FrameFocalPixels; }
    [[nodiscard]] float GetFrameLODThreshold() const { return m_FrameLODThreshold; }
    [[nodiscard]] u32   GetFrameHiZMipCount()  const { return m_FrameHiZMipCount; }
    [[nodiscard]] bool  GetFrameHiZRequested() const { return m_FrameHiZRequested; }
    [[nodiscard]] bool  GetFrameHiZTextureBound() const { return m_FrameHiZTextureBound; }
    /// 【P0 修复】本帧 Hi-Z 采样 UV 是否按"负高度视口"翻转（读数 `hiz_flip` 的真值来源）
    [[nodiscard]] bool  GetFrameHiZFlip() const { return m_FrameHiZFlip; }
    [[nodiscard]] const float* GetFrameViewProjRows() const { return m_FrameViewProjRows; }

    /// 录制 `Nanite_Cull` pass：
    ///   ① CPU 侧每帧重置三个计数/命令缓冲（沿用引擎既有的 `Map` 清零写法）
    ///   ② 上传 N 条假簇
    ///   ③ Dispatch（每簇一个线程）
    ///   ④ 插入 `ComputeShader → DrawIndirect` 屏障
    void RecordCullPass(rhi::IRHICommandList* cmd);

    // ── 绘制端 / 读回所需的缓冲访问（模块内部使用，外部不得越过 NaniteRenderer）──
    [[nodiscard]] rhi::IRHIBuffer* GetFakeClusterBuffer()  const { return m_FakeClusterBuf.get(); }
    [[nodiscard]] rhi::IRHIBuffer* GetIndirectCmdBuffer()  const { return m_IndirectCmdBuf.get(); }
    [[nodiscard]] rhi::IRHIBuffer* GetCountBuffer()        const { return m_CountBuf.get(); }
    [[nodiscard]] rhi::IRHIBuffer* GetRasterCountBuffer()  const { return m_RasterCountBuf.get(); }
    [[nodiscard]] u32 GetMaxFakeClusters() const { return kNaniteMaxFakeClusters; }

private:
    /// 每帧重置：计数缓冲清零 / 间接命令缓冲填哨兵 / 光栅化簇计数清零
    /// （CPU 侧 Map 写入；与 `GPUCulling::DispatchPhase2` 的清零写法一致，不发明新同步机制）
    void ResetFrameBuffers();
    /// 把 N 条假簇写进输入缓冲（N 变化或首帧时才需要，但每帧写一遍成本可忽略）
    void UploadFakeClusters();

    /// 【任务 13】实例剔除缓冲的**启动初值**（`Initialize` 调一次）：计数清零 + 列表填哨兵。
    /// 【不要**每帧**调它】每帧的计数清零是命令缓冲里的 4B 拷贝（GPU 有序）——录制期的主机写
    ///   会与派发竞争（CPU 领先 GPU ⇒ 两帧原子累加叠加，实测读回恰为 CPU 参考的 2 倍），
    ///   完整实测与根因见 `NaniteCull.cpp` 的 `RecordInstanceCullPass`。
    void ResetInstanceCullBuffers();
    /// 【任务 13】按 NDC 网格 + 反投影生成合成实例表与包围球（口径见 `SetInstanceCullFrame`）
    void BuildTestInstances();
    /// 【任务 13】把合成实例表与包围球写进各自的 GPU 缓冲
    void UploadInstanceCullInputs();
    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width  = 0;
    u32 m_Height = 0;

    /// 本帧假簇数量（任务 3 的输入；默认与 `NaniteSettings::fakeClusters` 一致）
    u32 m_FakeClusterCount = 6;

    // ── 任务 3 自持的四个缓冲 ──
    std::unique_ptr<rhi::IRHIBuffer> m_FakeClusterBuf;  // 假簇输入（Storage，CPU 每帧写）
    std::unique_ptr<rhi::IRHIBuffer> m_IndirectCmdBuf;  // 间接命令（Storage|Indirect，GPU 写）
    std::unique_ptr<rhi::IRHIBuffer> m_CountBuf;        // 命令条数（Storage|Indirect，GPU 原子写）
    std::unique_ptr<rhi::IRHIBuffer> m_RasterCountBuf;  // 已光栅化簇数（Storage，片元原子写）

    // ── compute 管线 ──
    rhi::ShaderBytecode m_CS;   // Nanite_Cull.comp.spv
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;

    // ============================================================
    // §14.8 任务 13：实例剔除自持资源与状态
    // ============================================================

    /// 本帧合成实例条数（`SetInstanceCullFrame` 的钳制后入参；默认 0 = 尚未设置）
    u32 m_TestInstanceCount = 0;

    // ── 四个自持缓冲（容量都是 `kNaniteMaxTestInstances`）──
    std::unique_ptr<rhi::IRHIBuffer> m_InstanceBuf;             // 128B 实例表（Storage，CPU 每帧写）
    std::unique_ptr<rhi::IRHIBuffer> m_InstanceSphereBuf;       // 16B 包围球（Storage，CPU 每帧写）
    std::unique_ptr<rhi::IRHIBuffer> m_VisibleInstanceBuf;      // 可见实例列表（Storage，GPU 原子压缩写）
    std::unique_ptr<rhi::IRHIBuffer> m_VisibleInstanceCountBuf; // 可见实例计数（Storage|TransferDst，GPU 原子写）
    /// 【任务 13】计数清零的**源**缓冲（常驻 0，TransferSrc）：
    ///   每帧开头的"计数清零"是命令缓冲内的 4B 拷贝（GPU 有序），而不是录制期的主机写。
    std::unique_ptr<rhi::IRHIBuffer> m_VisibleCountClearBuf;

    // ── 实例剔除 compute 管线 ──
    rhi::ShaderBytecode m_InstanceCullCS;   // Nanite_InstanceCull.comp.spv
    rhi::DescriptorSetLayoutHandle m_InstanceCullLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_InstanceCullSet    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIPipelineState> m_InstanceCullPSO;

    // ── 帧状态（CPU 侧；`RecordInstanceCullPass` 用它们做参考剔除）──
    NaniteFrustumPlanes m_FrameFrustum{};      // 本帧视锥（由 viewProj 提取）
    float4x4            m_FrameViewProj{1.0f}; // 本帧 view-proj（反投影合成实例网格用）
    float3              m_FrameCameraPos{0.0f};// 本帧相机世界坐标（网格深度/半径基准）
    NaniteInstanceCullParams m_InstanceCullParams{};  // 最近一次 push constant（含 planes + count）

    // ── 【任务 15】本帧的 LOD/Hi-Z 参数与状态 ──
    /// view-proj 的 4 个行（`m_FrameViewProjRows[r*4+c] = viewProj[c][r]`）——
    /// 与 GPU 参数缓冲里的 `vpRows` **同一份比特**（Hi-Z 投影用）
    float m_FrameViewProjRows[16] = { 0.0f };
    float m_FrameFocalPixels  = 0.0f;   ///< 像素焦距（0 ⇒ 关闭 LOD 选择）
    float m_FrameLODThreshold = kNaniteLODThresholdPixels;
    u32   m_FrameScreenW = 0u;          ///< 屏幕宽（Hi-Z 选层）
    u32   m_FrameScreenH = 0u;          ///< 屏幕高
    bool  m_FrameHiZRequested = false;  ///< 本帧调用方是否请求了 Hi-Z（cfg `nanite_hiz`）
    bool  m_FrameHiZTextureBound = false; ///< 本帧是否真的绑定了外部 Hi-Z 纹理（否则用占位）
    u32   m_FrameHiZMipCount = 0u;      ///< 本帧实际传给 shader 的 Hi-Z 层数（<2 ⇒ 遮挡关闭）
    /// 【P0 修复】本帧 Hi-Z 采样 UV 的 y 翻转真值（= `NaniteSettings::hizFlip`）——
    ///   读数行里的 `hiz_flip=` 直接打印它，避免"日志说翻转了、shader 其实没翻"。
    bool  m_FrameHiZFlip = true;
    NaniteCullChainParams m_ChainParams{};  ///< 最近一次上传的参数（回读/参考共用）

    // 合成实例表与包围球（CPU 侧镜像；每帧按 `m_TestInstanceCount` 重建/上传）
    std::vector<NaniteInstanceGpuObject> m_TestInstances;
    std::vector<NaniteInstanceSphere>    m_TestSpheres;
    // CPU 参考剔除结果（升序可见下标）—— dump 帧与 GPU 读回逐项比较
    std::vector<u32> m_CpuVisibleInstances;
    /// 【任务 15】CPU 侧的可见实例掩码（由 `m_CpuVisibleInstances` 展开；与 GPU 掩码同语义）
    std::vector<u32> m_CpuVisibleMask;

    // ============================================================
    // §14.8 任务 14：per-instance cluster BVH 的自持资源与状态
    // ============================================================

    /// BVH 是否已入库（`SetClusterBVH` 成功）。未入库 ⇒ `RecordCullChainPass` 直接跳过 Phase 2/3。
    bool m_BVHReady = false;

    /// CPU 侧 BVH 镜像（节点/叶子簇表/簇球/读数）——GPU 侧三个只读缓冲由它上传；
    /// 同时是 CPU 参考遍历的输入 ⇒ **GPU 与 CPU 读的是同一份比特**。
    NaniteClusterBVH m_BVHData;

    /// 【任务 15】CPU 侧 LOD 元数据镜像（与 `m_BVHData.clusterSpheres` 同序、同长度）
    std::vector<NaniteClusterLODInfo> m_LODInfo;

    /// 【§14.8 任务 26】每簇 BVH 节点深度的 CPU 镜像（懒计算；`EnsureClusterBVHDepths` 填充）
    /// 【为什么不在 `SetClusterBVH` 里顺手算掉】默认档永远不需要它；把一次 O(N) 遍历放进
    ///   资产上传路径会给**所有**档位加成本，而调试可视化是少数档位才用的东西。
    std::vector<u32> m_BVHClusterDepths;
    /// 深度镜像是否已经算过（只算一次；`Shutdown`/重新入库时复位）
    bool m_BVHDepthsComputed = false;

    /// 【任务 16】CPU 侧绘制参数镜像（与簇记录同序、同长度）——GPU 侧的只读表由它上传，
    ///   CPU 参考打包与 GPU 打包因此读的是**同一份比特**
    std::vector<NaniteClusterDrawRange> m_ClusterDrawRanges;

    /// 【任务 16】本帧的绘制容量（`SetDrawCapacity` 的钳制结果；默认 = 容量上界）
    u32 m_FrameDrawCapacity = kNaniteMaxIndirectDraws;

    /// 本帧实例域 = `min(合成实例数, kNaniteMaxBVHInstances)`（CPU 参考与 GPU 同口径）
    u32 m_BVHInstanceDomain = 0u;

    // ── 三个只读输入缓冲（容量固定，`SetClusterBVH` 一次性上传）──
    std::unique_ptr<rhi::IRHIBuffer> m_BVHNodeBuf;     // 节点（32B/条，容量 kNaniteMaxBVHNodes）
    std::unique_ptr<rhi::IRHIBuffer> m_BVHLeafBuf;     // 叶子簇表（u32/条，容量 kNaniteMaxBVHClusters）
    std::unique_ptr<rhi::IRHIBuffer> m_BVHSphereBuf;   // 簇球（16B/条，容量 kNaniteMaxBVHClusters）
    /// 【任务 15】LOD 元数据（16B/条，容量 kNaniteMaxBVHClusters）
    std::unique_ptr<rhi::IRHIBuffer> m_LODInfoBuf;
    /// 【任务 16】每簇绘制参数（16B/条，容量 kNaniteMaxBVHClusters；`SetClusterBVH` 一次性上传）
    std::unique_ptr<rhi::IRHIBuffer> m_ClusterDrawRangeBuf;
    // ── 每帧由 GPU 写的可写缓冲 ──
    std::unique_ptr<rhi::IRHIBuffer> m_VisibleClusterBuf;       // 可见簇引用（8B/条，容量 kNaniteMaxVisibleClusterRefs）
    std::unique_ptr<rhi::IRHIBuffer> m_VisibleClusterCountBuf;  // 可见簇计数（u32；Phase 3 的最终计数）
    /// 【任务 16】间接绘制命令（20B/条，容量 `kNaniteMaxIndirectDraws`）。**必须带 `Indirect`
    ///   usage**：`vkCmdDrawIndexedIndirectCount` 会把整块当命令缓冲读。
    std::unique_ptr<rhi::IRHIBuffer> m_IndirectDrawBuf;
    /// 【任务 16】绘制计数（u32）。**必须带 `Indirect` usage**（它是 `DrawIndexedIndirectCount`
    ///   的 countBuffer）；`TransferDst` 是每帧"命令缓冲内清零"拷贝的目标。
    std::unique_ptr<rhi::IRHIBuffer> m_DrawCountBuf;
    /// 【任务 15】可见实例掩码（u32/实例；Phase 1 每线程写 0/1，**不需要清零**）
    std::unique_ptr<rhi::IRHIBuffer> m_VisibleMaskBuf;
    /// 【任务 15】三阶段读数（扁平 u32，容量 `kNaniteCullStatsCapacity`）
    std::unique_ptr<rhi::IRHIBuffer> m_CullStatsBuf;
    /// 【任务 15】三阶段参数（112B；CPU 每帧上传）
    std::unique_ptr<rhi::IRHIBuffer> m_ChainParamBuf;
    /// 【任务 15】各计数的清零源（96B 常驻 0，TransferSrc）：
    ///   【0,4) 可见实例计数；【4,8) 任务 3 的命令条数；【8,12) 任务 3 的光栅化簇计数；
    ///   【12,16) 可见簇计数；【16,80) 三阶段读数（64B）；【80,84) 任务 16 的绘制计数。
    ///   **每帧的清零都在命令缓冲内**（不用主机写）。
    std::unique_ptr<rhi::IRHIBuffer> m_ClearZeroBuf;
    /// 【任务 15】Hi-Z 占位纹理（1×1 R32_FLOAT）+ 采样器：当本帧没有外部 Hi-Z 纹理时占位，
    ///   保证 binding 10 永远是**合法描述符**（Vulkan 不接受"未绑定/空纹理"的采样器绑定）。
    ///   层数传 0 ⇒ shader 的 `hizOccluded` 第一句就返回 false ⇒ 不会真的采样它。
    std::unique_ptr<rhi::IRHITexture> m_HiZPlaceholderTex;
    std::unique_ptr<rhi::IRHISampler> m_HiZSampler;

    // ── cluster BVH 遍历的 compute 管线 ──
    rhi::ShaderBytecode m_BVHCS;   // Nanite_ClusterBVH.comp.spv
    rhi::DescriptorSetLayoutHandle m_BVHLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_BVHSet    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIPipelineState> m_BVHPSO;

    // ============================================================
    // §14.8 任务 15：模块自持的 Hi-Z 金字塔构建（复用既有纹理与口径；为什么自建见头注释）
    // ============================================================

    /// 目标层（mip1..mipCount-1）的存储视图；由 `EnsureHiZBuildViews` 按纹理缓存/重建。
    /// 【为什么要缓存】视图要随"纹理被重建（窗口尺寸变化）"一起重建，而每次创建/销毁都有成本。
    void* m_HiZDestViews[kNaniteHiZBuildMaxViews] = { nullptr };
    /// 这些视图属于哪张纹理 / 建了几层（纹理指针变了就整批重建）
    rhi::IRHITexture* m_HiZViewOwner = nullptr;
    u32               m_HiZViewCount = 0u;
    /// 建/重建视图（幂等；`pyramid` 为空或层数不足时把可用视图数记下）
    void EnsureHiZBuildViews(rhi::IRHITexture* pyramid);

    rhi::ShaderBytecode m_HiZBuildCS;   // Nanite_HiZDownsample.comp.spv（与既有 HiZDownsample 同口径）
    rhi::DescriptorSetLayoutHandle m_HiZBuildLayout = rhi::kInvalidLayout;
    /// **每个目标 mip 一个专属描述符集**：这是本实现正确的关键 —— 各集合的每个绑定每帧只写一次，
    ///   因此不受"GPU 在执行期读描述符、最后一次主机写生效"这条次序规则影响。
    rhi::DescriptorSetHandle m_HiZBuildSets[kNaniteHiZBuildMaxViews] = { rhi::kInvalidSet };
    std::unique_ptr<rhi::IRHIPipelineState> m_HiZBuildPSO;
};

} // namespace he::render
