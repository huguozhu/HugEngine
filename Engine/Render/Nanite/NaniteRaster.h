#pragma once

// ============================================================
// Nanite/NaniteRaster.h — 光栅端（消费计数 → 间接绘制链；任务 16 起消费**可见簇列表**）
//
// 【§14.8 任务 3：绘制端（消费计数）】
//   任务 1 只有生命周期桩；任务 3 起本类做出一条**极小**的绘制通道：
//     · 用新的 RHI 接口 `IRHICommandList::DrawIndexedIndirectCount` 消费
//       `NaniteCull` 写出的「计数缓冲 + 间接命令缓冲」——绘制条数由 GPU 决定；
//     · 渲染目标是**模块自建的 1×1 R8 小目标**（不是 GBuffer 的任何附件），
//       片元着色器每被光栅化一个簇就把"已光栅化簇数"原子加一。
//   因此本 pass 对可见画面零影响（§14.2 不变式 1）。
//
// 【§14.8 任务 16：改吃"可见簇列表"写出的命令（默认路径）】
//   过去绘制的输入是任务 3 的**假簇链**（N 条固定命令）；任务 16 起默认改成
//   `Nanite_ClusterBVH.comp.slang` 在选出可见簇时**同一个原子槽位**写出的真实命令：
//     · 间接命令缓冲 = `NaniteCull::GetIndirectDrawBuffer()`（20B/条，与可见簇引用同槽位）
//     · 计数缓冲     = `NaniteCull::GetDrawCountBuffer()`（只在"真的写了命令"时 +1）
//     · maxDrawCount = `NaniteCull::GetMaxIndirectDraws()`（容量上界）
//   【无空转】命令与计数由**同一次派发**写出、绘制只覆盖 `[0, count)`、计数每帧在命令缓冲内
//   清 0（不用主机写 —— 主机写会与派发竞争，任务 13 实测错读成两倍）；三件事合起来保证
//   "画了 k 条命令 ⇔ k 个可见簇"，既不画没写的槽位，也不残留上一帧的命令。
//   【假簇链】仍完整保留（任务 3 的自证通道 + 可见链不可用时的退化路径），由
//   `NaniteSettings::fakeChain` 决定**谁来画**；两条路都复用本类的同一段录制代码。
//
// 【任务 4 起】这里才会出现真正写 GBuffer 的软光栅；按 §14.5 的裁决（A1/A2）
//   建模块自己的 PSO/附件布局，直接写既有 GBuffer 纹理句柄。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法）；不得依赖 `MeshBatcher` 的运行时状态
//   —— 它只当**一次性输入**。
// ============================================================

#include "RHI/RHI.h"

#include "Nanite/NaniteStream.h"   // 【任务 24】流式视图（页池/页表/簇→页/反馈环）
#include "Nanite/NaniteTypes.h"   // 【任务 18】NaniteSoftRasterParams / 读数槽位 / 深度键编码

#include <memory>
#include <span>   // 【任务 26】每簇 BVH 深度镜像的只读视图（可视化模式 4 的输入）

namespace he::render {

class NaniteRaster {
public:
    NaniteRaster() = default;
    ~NaniteRaster() = default;

    NaniteRaster(const NaniteRaster&) = delete;
    NaniteRaster& operator=(const NaniteRaster&) = delete;

    /// 建立绘制端自持资源：1×1 R8 目标 + 图形 PSO + 片元描述符集。
    /// @param rasterCountBuffer `NaniteCull` 自持的"已光栅化簇计数缓冲"（本类只引用，不持有）
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height,
                    rhi::IRHIBuffer* rasterCountBuffer);

    void Shutdown();
    void OnResize(u32 width, u32 height);

    [[nodiscard]] bool IsReady() const { return m_Device != nullptr && m_PSO != nullptr; }

    /// 录制绘制：
    ///   `DrawIndexedIndirectCount(indirectCmdBuffer, 0, countBuffer, 0, maxDrawCount, 20)`
    ///
    /// 【录在哪个帧图 pass 内】调用方（`NaniteRenderer`）把它录在**产出命令的那个 pass 体内**
    ///   （可见链 = `Nanite_CullChain3`；假簇链 = `Nanite_Cull`），紧跟在使用端之后：
    ///   帧图对"两个零资源 pass"的排序**不可依赖**（`TopologicalSort` 对 inDegree=0 的 pass
    ///   按 LIFO 处理），把绘制录进同一个 pass 体、用命令缓冲里的屏障定序，是唯一稳的写法
    ///   （任务 15 的教训与做法同源）。
    void RecordRasterPass(rhi::IRHICommandList* cmd,
                          rhi::IRHIBuffer* indirectCmdBuffer,
                          rhi::IRHIBuffer* countBuffer,
                          u32 maxDrawCount);

    /// 【§14.8 任务 16】告诉绘制端"占位索引缓冲必须覆盖的索引位置上界"（= 资产的索引总数）
    ///
    /// 【为什么需要占位索引缓冲】本通道仍是"数次数"的占位光栅（真实软光栅是任务 18），但
    ///   `DrawIndexedIndirectCount` 会拿命令里的 `firstIndex/indexCount` 去**绑定索引缓冲**取
    ///   索引 ⇒ 缓冲必须覆盖整个索引段的位置空间，否则是越界读（本设备**未**启用
    ///   `robustBufferAccess`，越界不是定义行为）。上界取"资产的索引总数"：
    ///   每个簇是索引段里的一个连续三角形区间 ⇒ `firstIndex + indexCount ≤ 索引总数`。
    /// 【调用时机与安全性】只在 `NaniteRenderer::EnsureAssetUploaded` 的一次性路径上调用
    ///   （那一刻 `NaniteScene::UploadPackedAsset` 已经 `WaitIdle` ⇒ 没有任何在飞的命令缓冲
    ///   引用旧缓冲，替换是安全的）；之后的帧直接复用，不再重建。
    /// 【钳制】`max(3, min(indexCount, kNanitePlaceholderIndexCountMax))` —— 至少放下假簇链的
    ///   一条命令，至多不超过"簇数上限 × 每簇三角形上限 × 3"这个可证上界。
    void SetPlaceholderIndexCapacity(u32 indexCount);

    /// 【§14.8 任务 16】读回"被光栅化的**绘制条数**"（真实 GPU 读回；每个绘制恰好 +1）。
    /// 【同步约定】调用方必须保证 GPU 已完成（样例 dump 路径已有 `WaitIdle()`）。
    [[nodiscard]] u32 ReadbackRasterCount();

    /// 占位索引缓冲当前覆盖的索引位置个数（dump/日志用）
    [[nodiscard]] u32 GetPlaceholderIndexCount() const { return m_PlaceholderIndexCount; }

    /// 【§14.8 任务 4：UAV 自证通道】录制 `Nanite_TestWrite` pass。
    /// 用 `RWTexture2D<float4>`（`Nanite_TestWrite.comp.slang`）往**既有 GBuffer albedo**
    /// 写 8×8 棋盘，证明 "compute 写既有 GBuffer（A1）且同帧被 Lighting 读到"。
    ///
    /// 【懒初始化】PSO 与描述符集在**首次真正录制时**才建（`EnsureTestWriteResources`）：
    ///   `testWrite` 默认关闭，关闭档下这些资源一个都不会创建（§14.2 不变式 1：
    ///   关闭时不产生新的每帧 CPU 开销，也不多建任何 GPU 资源）。
    ///
    /// 【资源只借用不持有】`albedo` 是 `GBufferRenderer` 的纹理，本类只把它绑成存储图像，
    ///   不参与其生命周期；分辨率也从纹理自身取（`GetWidth/GetHeight`）。
    void RecordTestWritePass(rhi::IRHICommandList* cmd, rhi::IRHITexture* albedo);

    // ============================================================
    // 【§14.8 任务 6】mesh PSO 自证通道（最小 mesh 管线 + 非空输出读回）
    //
    // 【为什么放在本类】任务 6 的产物是"另一条绘制端点"（mesh 管线替代 VS+IA），
    //   与既有软/间接绘制端同属 `NaniteRaster` 的职责（§14.3：本文件是"软光栅（后加硬光栅分支）"），
    //   故不新开文件、不扩大模块公共面。
    //
    // 【懒初始化】与任务 4 的 UAV 自证通道同理：`meshTest` 默认关，关闭档下 mesh 目标/缓冲/PSO
    //   一个都不会创建（§14.2 不变式 1：关闭时不产生新的每帧 CPU 开销，也不多建任何 GPU 资源）。
    //
    // 【设备要求】需要 `VK_EXT_mesh_shader`（`DeviceCaps::supportsMeshShaders`）。
    //   不支持时**不建 PSO、不记录**，并由 `IsMeshTestSupported()` 让帧图侧连 pass 都不注册
    //   —— 不做任何替代方案（与任务 3 对 `DrawIndexedIndirectCount` 的处理口径一致）。
    // ============================================================

    /// 设备是否具备 mesh shader 能力（`Initialize` 时查一次 `DeviceCaps::supportsMeshShaders`）。
    [[nodiscard]] bool IsMeshTestSupported() const { return m_MeshShaderSupported; }

    /// mesh PSO 是否真的建起来了（dump 帧日志 `mesh_pso=ok/fail` 的依据）。
    [[nodiscard]] bool IsMeshTestPSOReady() const { return m_MeshTestPSO != nullptr; }

    /// 录制 `Nanite_MeshTest` pass：`DrawMeshTasks(1,1,1)` 画进模块自建的 1×1 R8 目标，
    /// 并把该目标拷进 host 可见缓冲供 dump 帧读回（拷贝在 render pass 之外录制）。
    void RecordMeshTestPass(rhi::IRHICommandList* cmd);

    /// 读回 mesh 通道的「被光栅化图元数」（片元原子计数缓冲的真实 GPU 读回）。
    /// 【同步约定】调用方必须保证 GPU 已完成（样例 dump 路径已有 `WaitIdle()`）。
    [[nodiscard]] u32 ReadbackMeshTestOutputs();

    /// 读回 mesh 通道 1×1 R8 目标像素的**最大**值（`CopyTextureToBuffer` → Map 的真实 GPU 读回）。
    [[nodiscard]] u32 ReadbackMeshTestTargetMax();

    // ============================================================
    // 【§14.8 任务 18】软光栅：两趟「原子深度键 + 等值复检」写 GBuffer
    //
    // 【与设计 §5.2 的关系】§5.2 写的是"每簇一个 wave，interlock 写 GBuffer"（ROV）。本仓库的
    //   Slang 版本**在 compute 里静默丢弃 ROV 语义**（实测：exit 0、零诊断、SPIR-V 里没有
    //   任何 interlock；SPIR-V 规范规定 interlock 的 execution mode 只对 Fragment 合法）
    //   ⇒ 单趟写法有竞态。改用**两趟**：第 1 趟只做逐像素 `InterlockedMin(深度键)`，
    //   第 2 趟重跑光栅化 + 等值复检后才写颜色 ⇒ 每个像素恰好一个三角形写一次。
    //   完整证据见 `Nanite_SoftRasterCommon.slang` 头部与实施记录。
    //
    // 【深度】compute **不写** `D32_SFLOAT`：本机 NVIDIA 支持它做存储图像、同机 AMD 核显不支持
    //   （§14.14），且 GBuffer 深度纹理按任务 4 的 A1 裁决**没有** `UnorderedAccess` usage
    //   （本次改动面禁止改 `GBufferRenderer`）。故走 §14.5 裁决里的另一条路：
    //   模块自持一张**深度键**缓冲，最后一趟用全屏片元 + `SV_Depth` 把深度写进既有深度附件。
    //   运行时仍会查一次"该格式能不能做存储图像"（`IRHIDevice::SupportsStorageImage`）并打进度数，
    //   让降级是**被报告**的而不是静默的。
    // ============================================================

    /// 软光栅 / 清屏要用的既有 GBuffer 纹理集合（**只借用**，生命周期归 `GBufferRenderer`）
    struct GBufferTargets {
        rhi::IRHITexture* albedo      = nullptr;   ///< MRT0 RGBA16F（albedo.rgb + metallic.a）
        rhi::IRHITexture* normal      = nullptr;   ///< MRT1 RGBA16F（normal.xyz + roughness.a）
        rhi::IRHITexture* emissive    = nullptr;   ///< MRT2 RGBA16F（本任务只清、不写）
        rhi::IRHITexture* velocity    = nullptr;   ///< MRT3 RG16F （本任务只清、不写）
        rhi::IRHITexture* worldPos    = nullptr;   ///< MRT4 RGBA16F
        rhi::IRHITexture* disneyA     = nullptr;   ///< MRT5 RGBA16F（本任务只清、不写）
        rhi::IRHITexture* disneyB     = nullptr;   ///< MRT6 RGBA16F（本任务只清、不写）
        rhi::IRHITexture* lightmapKey = nullptr;   ///< MRT7 RGBA16F（uv.xy + Nanite 段页号）
        rhi::IRHITexture* depth       = nullptr;   ///< D32_SFLOAT（清 + 深度解析写入）

        [[nodiscard]] bool HasClearTargets() const {
            return albedo && normal && emissive && velocity && worldPos
                && disneyA && disneyB && lightmapKey && depth;
        }
        [[nodiscard]] bool HasSoftRasterTargets() const {
            return albedo && normal && worldPos && lightmapKey && depth;
        }
    };

    /// 软光栅要读的资产缓冲（**只读**；由 `NaniteScene` 持有，模块内借用）
    struct AssetViews {
        rhi::IRHIBuffer* clusters = nullptr;   ///< 簇记录段（64B/条）
        rhi::IRHIBuffer* vertices = nullptr;   ///< 量化顶点段（16B/条）
        rhi::IRHIBuffer* indices  = nullptr;   ///< 索引段（3×u16 进 u32[2]）
        rhi::IRHIBuffer* header   = nullptr;   ///< 文件头（取 meshMaxExtent 由调用方算好传入）
        /// 【任务 19】材质段（32B/条）。为 nullptr ⇒ 软光栅按 `materialCount = 0` 走中性兜底
        /// 并把像素计进 `fallback_pixels`（**不静默**）。
        rhi::IRHIBuffer* materials = nullptr;

        /// 【任务 24】流式视图（bindings 15..20）。
        /// 【为什么放在这里而不是另开一个参数】`RecordSoftRasterPass` / `RecordHardRasterPass`
        ///   已经收一个 `AssetViews`；流式是"同一批几何的另一个来源"，与资产视图同生共死
        ///   （关闭档 `stream.enabled == false`，绑定槽位复用既有资产缓冲作占位）。
        NaniteStreamViews stream;

        [[nodiscard]] bool valid() const { return clusters && vertices && indices; }
    };

    /// 【任务 19】材质读数（由 `NaniteRenderer` 在资产构建后一次性告知；软光栅日志打印它们）
    ///   `multiMeshClusters`  = 三角形跨越 ≥2 个源网格的簇数（按多数票归属）
    ///   `distinctMaterials`  = 材质段里内容各不相同的记录数
    ///   `texturedMaterials`  = 其中带 BaseColor 纹理的记录数
    void SetMaterialStats(u32 materialCount, u32 multiMeshClusters,
                          u32 distinctMaterials, u32 texturedMaterials) {
        m_MaterialCount      = materialCount;
        m_MultiMeshClusters  = multiMeshClusters;
        m_DistinctMaterials  = distinctMaterials;
        m_TexturedMaterials  = texturedMaterials;
    }
    [[nodiscard]] u32 GetMaterialCount() const { return m_MaterialCount; }

    /// **清屏**：用 compute 把 8 个颜色附件清成**既有路径的同一组清除值**，并用 RHI 的
    /// `ClearDepthStencil` 把深度清成远平面 —— **不画任何几何**（这就是"既有几何路径让位、
    /// 模块接管写入"的第一步）。
    /// 【为什么不是渲染通道清屏】`BeginOffscreenPassMRT` 的 loadOp 取自 PSO，而 render pass 在
    ///   RHI 里按格式组合复用（Decal 用同一组 8 格式 + `Load`）⇒ 实测清不掉（未覆盖像素读出
    ///   (0,0,0,0) 而不是清除值）。compute 写 UAV 完全在模块手里，不依赖 render pass 的缓存行为。
    void RecordGBufferClearPass(rhi::IRHICommandList* cmd, const GBufferTargets& targets);

    /// **软光栅**（三趟录在同一个命令缓冲里，用屏障定序）：
    ///   ① 4B/像素的 `CopyBuffer`（常驻 0xFF 源 → 深度键）= 每帧清成"没有几何"的哨兵；
    ///   ② 第 1 趟 compute（`Nanite_SoftRasterDepth.comp`）：逐簇逐三角形的原子深度键；
    ///   ③ 第 2 趟 compute（`Nanite_SoftRaster.comp`）：重跑光栅化 + 等值复检 → 写 4 张 GBuffer；
    ///   ④ 深度解析（全屏片元 `Nanite_DepthResolve.*`）：深度键 → `SV_Depth` 写既有深度附件。
    /// 顺序全部由函数内**显式**发出的屏障给出（帧图不跟踪模块自持资源，也排不动这些内部段）。
    void RecordSoftRasterPass(rhi::IRHICommandList* cmd,
                              const GBufferTargets& targets,
                              const AssetViews& asset,
                              const NaniteSoftRasterParams& params,
                              rhi::IRHIBuffer* visibleRefs,
                              rhi::IRHIBuffer* visibleCount,
                              rhi::IRHIBuffer* instances,
                              u32 visibleCapacity,
                              const float clearDepth);

    /// dump 帧打印**恰好一行**软光栅读数（真实 GPU 读回）：
    ///   `[Nanite] soft_raster clusters=<C> soft=<S> skipped_big=<B> triangles=<T> pixels_written=<P>
    ///    degenerate=<D> neutral_material_pixels=<N> depth_written=<0|1> depth_storage_image_supported=<0|1>
    ///    depth_src=<key+SV_Depth> ...`
    /// 【同步约定】与其它读回相同：只 Map，不等待；调用方必须已 `WaitIdle()`。
    void LogSoftRasterReadback();

    /// 【§14.8 任务 23】把**软光栅读数缓冲**原样读进 `out`（`kNaniteSoftStatsCapacity` 条扁平 u32）。
    ///
    /// 【为什么暴露原始数组而不是 5 个逐桶 getter】"5 个桶"在类型上不是 5 个各自独立的量，
    ///   而是**一段连续槽位 + 一组区间常量**（`kNaniteSoftStatSizeBucket0` 与
    ///   `kNaniteSizeBucketUpperBound`）；把整段交出去，读的人（`NaniteRenderer` 的 `size_dist` 行）
    ///   就能按同一组常量解释它，且新增槽位时不需要改本函数的签名。
    /// 【同步约定】与 `LogSoftRasterReadback` 相同：只 Map、不等待；调用方必须已 `WaitIdle()`。
    ///   缓冲不存在（软光栅未就绪）时把 `out` 清零后返回。
    void ReadbackSoftStats(u32 (&out)[kNaniteSoftStatsCapacity]);

    /// 【§14.8 任务 23】最近一次软光栅录制用的阈值（push constant 的真值，来自 `NaniteSettings`）。
    /// `size_dist` / `perf` 读数行打印它 —— 与 `soft_raster` 行的 `max_triangles` 同源同值。
    [[nodiscard]] u32 SoftLastMaxTriangles() const { return m_SoftLastMaxTriangles; }

    /// 【§14.8 任务 23】把**硬光栅读数缓冲**原样读进 `out`（`kNaniteHardStatsCapacity` 条）。
    /// 【为什么也要它】`perf` 行要把"分流"与"帧时"绑在同一行 ⇒ 需要硬光栅侧的接手簇数/像素数；
    ///   硬光栅未开（`hardRaster=0`）时缓冲不存在，本函数把 `out` 清零（`perf` 行的基线档因此
    ///   打印 `hard_clusters=0`，而不是缺字段）。
    void ReadbackHardStats(u32 (&out)[kNaniteHardStatsCapacity]);

    /// 软光栅是否就绪（资源 + PSO 都建起来了）
    [[nodiscard]] bool IsSoftRasterReady() const { return m_SoftColorPSO != nullptr; }

    // ============================================================
    // 【§14.8 任务 22】硬光栅：mesh shader 分流（大簇一侧）
    //
    // 【与设计 §5.2 的关系】§5.2 的分流判据是 **`cluster.triCount > 16` ⇒ 硬件（mesh shader）**。
    //   任务 18 的软光栅第 1 趟已经把这类簇 `return` 掉并计进 `kNaniteSoftStatSkippedClusters`
    //   （注释就写着"任务 22 的活"）⇒ 那个集合就是本通道的输入。
    //
    // 【次序：硬光栅排在软光栅**全部三趟之后**，两个方向的遮挡都正确】（这是本任务最关键的裁决，
    //   完整推导见 `RecordHardRasterPass` 的注释）：
    //     软光栅两趟写颜色 → 深度解析写 D32 → 硬光栅用 `LessEqual + depthWrite` 画；
    //     · 更近的硬片元通过深度测试并**覆盖**软颜色 ⇒ 硬遮软 ✓
    //     · 更远的硬片元被深度测试丢弃、软颜色保留 ⇒ 软遮硬 ✓
    //   正确性只取决于"**后写者**是否带深度测试"，与"谁先写"无关；反过来把硬光栅排在软光栅
    //   **之前**才是不成立的（软颜色趟的等值复检只对照软自己的深度键，看不见硬几何，
    //   会无条件覆盖更近的硬颜色）。
    //   已知边界（如实记录，不是"完全等价"）：
    //     ① 深度解析把软深度**截断到 24 位尾数**（`asfloat(key & 0xFFFFFF00)`），
    //        硬片元深度与软深度相差 < 256 ULP 的极窄带内比较可能给错胜负；
    //     ② 深度**恰好相等**时 `LessEqual` 让硬片元胜出（平局口径，任一侧都可辩护）；
    //     ③ 硬光栅会写深度附件 ⇒ 它之后的下游（Hi-Z / SSAO / SSR / 深度重建）看到的是
    //        "软+硬"的合成深度，而不是软光栅时代的"只有软、其余全远平面"。
    //        这正是硬光栅必须默认关闭的原因（见 `NaniteSettings::hardRaster`）。
    // ============================================================

    /// 硬光栅是否可用（设备支持 mesh shader **且** `DeviceCaps` 的四个上限容得下本通道的声明）
    [[nodiscard]] bool IsHardRasterSupported() const { return m_HardRasterCapable; }

    /// 硬光栅 PSO 是否真的建起来了（dump 帧日志 `pso=ok/fail` 的依据）
    [[nodiscard]] bool IsHardRasterReady() const { return m_HardRasterPSO != nullptr; }

    /// 懒建硬光栅资源（mesh PSO + 描述符集 + 读数缓冲）。
    /// 【为什么放在 public】与 `EnsureSoftRasterResources` 同一理由：`NaniteRenderer`（门面）在
    ///   **帧图构建期**调用它——帧图不为这个通道注册独立 pass，真正的绘制录在
    ///   `Nanite_CullChain3` 的 pass 体内；本函数只保证执行期资源就绪。
    /// 【懒建 + 门控】`hardRaster` 默认关 ⇒ 关闭档下 mesh PSO/描述符集/读数缓冲一个都不创建
    ///   （§14.2 不变式 1 的口径：关闭时不产生新的每帧开销，也不多建 GPU 资源）。
    /// @param targets 本帧的 GBuffer 纹理（用来定 8 个颜色附件的格式与描述符绑定）
    bool EnsureHardRasterResources(const GBufferTargets& targets);

    /// 录制硬光栅：**必须在 `RecordSoftRasterPass` 之后**调用（次序是正确性的前提，见上面的推导）。
    ///   · 颜色附件 `Load + writeMask`（只写 MRT0/1/4/7）、深度附件 `Load + LessEqual + write`
    ///     ⇒ 保留软光栅已写好的内容，只覆盖"更近"的硬像素；
    ///   · 任务数 = 可见簇容量（GPU 驱动的计数由 shader 的 `slot >= visibleCount` 早退收口，
    ///     与软光栅同一条既有口径）；
    ///   · 前后各发一组显式屏障（帧图不跟踪模块自持资源，也排不动这些内部段）。
    void RecordHardRasterPass(rhi::IRHICommandList* cmd,
                              const GBufferTargets& targets,
                              const AssetViews& asset,
                              const NaniteSoftRasterParams& params,
                              rhi::IRHIBuffer* visibleRefs,
                              rhi::IRHIBuffer* visibleCount,
                              rhi::IRHIBuffer* instances,
                              u32 visibleCapacity);

    /// dump 帧打印**恰好一行**硬光栅读数（真实 GPU 读回）：
    ///   `[Nanite] hard_raster clusters=<C> prims=<P> pixels=<X> fallback_pixels=<F>
    ///    soft_clusters=<S> soft_pixels=<Y> skipped_big=<B> hard_share_permille=<h>
    ///    soft_share_permille=<s> max_triangles=<T> mesh_supported=<0|1> pso=<ok|fail>`
    /// 【同步约定】与其它读回相同：只 Map，不等待；调用方必须已 `WaitIdle()`。
    void LogHardRasterReadback();

    /// 【任务 18】懒建软光栅资源（两趟 compute PSO + 深度解析 PSO + 三套描述符集布局）。
    /// 【为什么放在 public】`NaniteRenderer`（门面）在**帧图构建期**调用它：帧图不注册这个 pass，
    ///   本函数只保证执行期的资源就绪；真正的派发录在 `Nanite_CullChain3` 的 pass 体内。
    /// 描述符集只建一次；**每次录制**再把绑定重写一遍（资产/可见簇/实例缓冲与 4 张颜色目标）。
    /// @param targets 本帧的 GBuffer 纹理（用来建第 2 趟的 4 张颜色目标绑定）
    bool EnsureSoftRasterResources(const GBufferTargets& targets);

    // ============================================================
    // 【§14.8 任务 26 / §14.34 末尾最小范围第 1 条】屏幕可视化（debug view）
    //
    // 【四项验收明文点名的可视化】可见簇数 / 软硬光栅占比 / LOD 层级 / BVH 深度；由
    //   `NaniteSettings::debugView` 切换（**默认 0 = 关**）。
    //
    // 【落点：模块自建小目标，不改任何 GBuffer/既有渲染目标】一张 64×32 的 `R32_UINT`
    //   （8 KB）+ 一个 8 KB 读回缓冲 + 一个 compute PSO + 一个描述符集。关闭档**一个都不建**
    //   （§14.2 不变式 1：既不多建 GPU 资源、也不多一行日志）。
    //   口径（一像素 = 一个屏幕 tile、四种模式的编码）写在 `NaniteTypes.h` 的
    //   `kNaniteDebugView*` 与 `Nanite_DebugView.comp.slang` 的文件头注释里。
    //
    // 【为什么不在帧图里注册一个 pass】它只读剔除链的输出（可见簇列表/计数）与三张 CPU 一次性
    //   上传的只读表，写的是模块自持目标 ⇒ 与硬光栅同理，录在 `Nanite_CullChain3` 的 pass 体内、
    //   用命令缓冲里的屏障定序，是唯一稳的写法（帧图对零帧图资源的 pass 排序不可依赖）。
    // ============================================================

    /// 可视化是否可用（目标 + PSO + 描述符集都建起来了）
    [[nodiscard]] bool IsDebugViewReady() const { return m_DebugViewPSO != nullptr; }

    /// 懒建可视化的目标/读回缓冲/描述符集/PSO（只在 `debugView != 0` 时被调用）。
    /// @param mode          档位（1..4；0 不建任何东西）
    /// @param assetClusters 【模式 2】资产簇记录段（64B/条；只为取 `triangleCount` 做软/硬分类）
    /// @param spheres       簇包围球表（16B/条，**按资产簇下标**；与 BVH 遍历同一份比特）
    /// @param lodInfo       每簇 LOD 元数据（16B/条，`lodLevel` 是模式 3 的唯一输入）
    /// @param bvhDepths     每簇 BVH 节点深度（u32/条；CPU 由同一棵树逐叶展开，模式 4 的唯一输入）
    /// 【失败】设备缺失 / 档位为 0 / 输入缓冲为空 ⇒ 返回 false（调用方跳过本帧的可视化，不崩）
    bool EnsureDebugViewResources(u32 mode,
                                  rhi::IRHIBuffer* assetClusters,
                                  rhi::IRHIBuffer* spheres,
                                  rhi::IRHIBuffer* lodInfo,
                                  std::span<const u32> bvhDepths);

    /// 录制可视化：先清屏（`mode == 0` 的内部趟）再按档位累加，最后把目标拷进 host 可见缓冲。
    /// 【必须在剔除链之后调用】它消费 `visibleRefs` / `visibleCount`（同一帧 GPU 刚写出的列表）。
    /// 【同步】内部发三条显式屏障（清屏前 / 清屏→累加 / 累加→拷贝），帧图不跟踪模块自持资源。
    void RecordDebugViewPass(rhi::IRHICommandList* cmd,
                             rhi::IRHIBuffer* visibleRefs,
                             rhi::IRHIBuffer* visibleCount,
                             rhi::IRHIBuffer* instances,
                             u32 visibleCapacity,
                             const NaniteDebugViewParams& params);

    /// dump 帧打印**恰好一行**可视化读数（把 64×32 目标读回 host 后统计）：
    ///   `[Nanite] debug_view mode=<1..4> name=<...> px_nonzero=<n> px_nonzero_permille=<p>
    ///    distinct_vals=<d> panelA=[sum max nonzero distinct] panelB=[sum max nonzero] …`
    /// 【"不是黑屏"的判据就在这里】`px_nonzero` / `distinct_vals` / 各面板的 sum/max 都是
    ///   **真实 GPU 读回**（`CopyTextureToBuffer → Map`）；四种模式若产出同一张图，
    ///   这些数会完全相同（报告里逐档对比即可发现）。
    /// @param visible 本帧可见簇数（来自剔除链的计数缓冲；用来给出"落在屏幕外的簇数"）
    /// 【门控】`debugView == 0` 时直接返回：不 Map、不打一个字符。
    void LogDebugViewReadback(u32 visible);


    /// 深度键缓冲当前覆盖的像素数（= 宽×高；dump/日志用）
    [[nodiscard]] u32 GetDepthKeyPixelCount() const { return m_DepthKeyPixels; }

private:
    /// 懒建 `Nanite_TestWrite` 的 PSO + 描述符集布局（首次录制时调用一次）。
    /// 返回 false 表示创建失败（调用方跳过本次录制，不影响其它 pass）。
    bool EnsureTestWriteResources();

    /// 【任务 16】懒建/重建占位索引缓冲（首次录制或容量变大时调用）。
    /// 返回 false 表示创建失败（调用方跳过本次绘制，不崩）。
    bool EnsurePlaceholderIndexBuffer();

    /// 【任务 16】创建占位索引缓冲并填入 `0,1,2,0,1,2,…` 的周期模式
    /// （为什么是这个模式：见 `Nanite_Raster.vert.slang` 的"任务 16 的改动"一节）
    bool CreatePlaceholderIndexBuffer(u32 indexCount);

    /// 懒建 `Nanite_MeshTest` 的目标/缓冲/描述符集/mesh PSO（首次录制时调用一次）。
    /// 返回 false 表示创建失败或设备不支持 mesh shader。
    bool EnsureMeshTestResources();

    /// 【任务 18】按当前视口重建深度键缓冲与其常驻 0xFF 源（容量变化时）。
    bool EnsureDepthKeyBuffers(u32 width, u32 height);

    /// 【任务 18】在命令缓冲内清深度键（`CopyBuffer` 常驻 0xFF 源 → 深度键整块）
    void RecordDepthKeyClear(rhi::IRHICommandList* cmd);

    /// 【任务 18】在命令缓冲内清软光栅读数（`CopyBuffer` 常驻 0 源 → 读数缓冲整块）
    void RecordSoftStatsClear(rhi::IRHICommandList* cmd);

    /// 【任务 22】在命令缓冲内清**硬光栅**读数（模块自持的小缓冲；同一个 `CopyBuffer` 惯例）
    void RecordHardStatsClear(rhi::IRHICommandList* cmd);

    /// 【任务 18】懒建清屏 compute 的 PSO + 描述符集布局（8 张颜色目标的 UAV）
    bool EnsureGBufferClearResources();

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width  = 0;
    u32 m_Height = 0;

    /// 模块自建的小目标（R8，1×1）。它**不在** GBuffer 里，写它不会改变可见画面。
    std::unique_ptr<rhi::IRHITexture> m_Target;
    /// 绘制端不读顶点属性，但 `DrawIndexedIndirectCount` 仍会绑定顶点缓冲
    std::unique_ptr<rhi::IRHIBuffer>  m_DummyVB;
    /// 【任务 16】占位**索引**缓冲：覆盖资产的整个索引位置空间，内容 = `0,1,2` 周期模式。
    /// 懒建（首次录制时按 `m_PlaceholderIndexCount` 建，资产上传后由
    /// `SetPlaceholderIndexCapacity` 放大一次）。
    std::unique_ptr<rhi::IRHIBuffer>  m_PlaceholderIB;
    /// 占位索引缓冲覆盖的索引位置个数（u32 元素数）
    u32 m_PlaceholderIndexCount = 0u;
    /// 【任务 16】"绘制计数"缓冲的**非持有**引用（由 `NaniteCull` 创建并持有；`Initialize` 传入）。
    /// 本类只在 dump 帧读回它（清零在 `NaniteCull::RecordCullPass` 里，那个 pass 一定先执行）。
    rhi::IRHIBuffer* m_RasterCountRef = nullptr;

    rhi::ShaderBytecode m_VS;   // Nanite_Raster.vert.spv
    rhi::ShaderBytecode m_FS;   // Nanite_Raster.frag.spv

    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;

    /// 最近一次 pass 传入的 maxDrawCount（诊断用）
    u32 m_LastMaxDrawCount = 0;

    // ── §14.8 任务 4：UAV 自证通道（懒建；testWrite 关闭时全部为空）──
    rhi::ShaderBytecode            m_TestWriteCS;                                  // Nanite_TestWrite.comp.spv
    rhi::DescriptorSetLayoutHandle m_TestWriteLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_TestWriteSet    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIPipelineState> m_TestWritePSO;

    // ── §14.8 任务 6：mesh PSO 自证通道（懒建；meshTest 关闭时全部为空）──
    /// 设备能力（`Initialize` 时查询一次；false ⇒ 永不建 mesh PSO，也不注册 pass）
    bool m_MeshShaderSupported = false;

    rhi::ShaderBytecode m_MeshTestMS;   // Nanite_MeshTest.mesh.spv
    rhi::ShaderBytecode m_MeshTestFS;   // Nanite_MeshTest.frag.spv

    /// 模块自建的 1×1 R8 小目标：**不是** GBuffer 的任何附件 ⇒ 改不动可见画面。
    /// usage 比任务 3 的目标多 `TransferSrc`（任务 6 要把它读回 host）与 `ShaderResource`
    /// （`CopyTextureToBuffer` 拷完无条件还原成 SHADER_READ_ONLY；缺 SAMPLED 位会报
    /// VUID-VkImageMemoryBarrier-oldLayout-01211）。
    std::unique_ptr<rhi::IRHITexture> m_MeshTestTarget;
    /// 片元原子计数：本帧 mesh shader 真正输出并被光栅化的图元数（dump 帧读回）
    std::unique_ptr<rhi::IRHIBuffer>  m_MeshTestCount;
    /// 1×1 R8 目标 → host 的读回缓冲（dump 帧 Map；照仓库既有 buffer 读回写法）
    std::unique_ptr<rhi::IRHIBuffer>  m_MeshTestReadback;

    rhi::DescriptorSetLayoutHandle m_MeshTestLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_MeshTestSet    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIPipelineState> m_MeshTestPSO;

    // ── §14.8 任务 18：软光栅（懒建；软光栅关闭或资产未就绪时为空）──
    /// 三个入口的字节码：两趟 compute + 深度解析的 VS/FS
    rhi::ShaderBytecode m_SoftDepthCS;   // Nanite_SoftRasterDepth.comp.spv
    rhi::ShaderBytecode m_SoftColorCS;   // Nanite_SoftRaster.comp.spv
    rhi::ShaderBytecode m_DepthResolveVS;  // Nanite_DepthResolve.vert.spv
    rhi::ShaderBytecode m_DepthResolveFS;  // Nanite_DepthResolve.frag.spv

    /// 两趟 compute 共用的描述符集布局（bindings 0..7）与**每趟一个**的描述符集：
    /// 【为什么两趟各一个集合】第 2 趟多绑 4 张 GBuffer 颜色目标（binding 8..11），
    ///   且引擎的 GPU 在**执行期**读描述符、最后一次主机写对整段命令缓冲生效（任务 15 的教训）
    ///   ⇒ 两次派发必须在**不同的**集合上，且每个集合每帧只写一次。
    rhi::DescriptorSetLayoutHandle m_SoftDepthLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_SoftDepthSet    = rhi::kInvalidSet;
    rhi::DescriptorSetLayoutHandle m_SoftColorLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_SoftColorSet    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIPipelineState> m_SoftRasterPSO;      // 第 1 趟（深度键）
    std::unique_ptr<rhi::IRHIPipelineState> m_SoftColorPSO;       // 第 2 趟（写 GBuffer）

    /// 深度解析通道（全屏片元；只写深度附件，无颜色附件）
    rhi::DescriptorSetLayoutHandle m_DepthResolveLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_DepthResolveSet    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIPipelineState> m_DepthResolvePSO;

    /// 【任务 18】清屏 compute（8 张颜色目标的存储图像绑定 + 128B 清除值 push constant）
    rhi::ShaderBytecode m_ClearCS;   // Nanite_GBufferClear.comp.spv
    rhi::DescriptorSetLayoutHandle m_ClearLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_ClearSet    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIPipelineState> m_ClearPSO;

    /// 深度键缓冲（`RWStructuredBuffer<uint>`，W×H 条）+ 每帧清零用的常驻 0xFF 源
    /// 【为什么用缓冲而不是 R32_UINT 存储图像】结构化缓冲上的 `InterlockedMin` 同样零额外设备
    ///   特性，而且**清屏等价于一次 `CopyBuffer`**（存储图像没有等价的"整图填充"命令，
    ///   否则要再写一个小 compute）。代价是 4B/像素（1920×1080 ≈ 8.3MB）。
    std::unique_ptr<rhi::IRHIBuffer> m_DepthKey;
    std::unique_ptr<rhi::IRHIBuffer> m_DepthKeyZeroSrc;   ///< 常驻 0xFF（`TransferSrc`）
    u32 m_DepthKeyPixels = 0u;                            ///< 当前容量（像素数）；尺寸变化时重建
    u32 m_DepthKeyZeroSrcPixels = 0u;                     ///< 常驻 0xFF 源的容量（与深度键同步重建）

    /// 读数缓冲（扁平 u32，`kNaniteSoftStatsCapacity` 条）+ 每帧清零用的常驻 0 源
    std::unique_ptr<rhi::IRHIBuffer> m_SoftStats;
    std::unique_ptr<rhi::IRHIBuffer> m_SoftStatsZeroSrc;

    /// 上一次录制时记下的参数（dump 帧日志用：真实 GPU 读回 + CPU 侧真值对照）
    u32 m_SoftLastMaxTriangles  = 0u;
    u32 m_SoftLastInstanceCount = 0u;

    // ── 【任务 19】材质接入的读数与状态 ──
    /// 材质段条数（= `NaniteScene::AssetBuffers::materials` 的记录数；0 ⇒ 中性兜底）
    u32 m_MaterialCount     = 0u;
    u32 m_MultiMeshClusters = 0u;   ///< 跨源网格的簇数（多数票归属）
    u32 m_DistinctMaterials = 0u;   ///< 材质段里内容各不相同的记录数
    u32 m_TexturedMaterials = 0u;   ///< 其中带 BaseColor 纹理的记录数
    /// 【任务 19】第 2 趟的描述符集是否已登记到 bindless 堆（只登记一次；登记后强制一次 Flush）
    bool m_BindlessRegistered = false;

    /// 【运行时格式能力】**GBuffer 深度格式能否做存储图像**（启动时查一次；
    ///   `IRHIDevice::SupportsStorageImage(Format::D32_FLOAT)`）。它决定"compute 写深度"这条
    ///   备选路线可不可用 —— 本实现走 `SV_Depth`，但读数里必须把这个能力标出来（§14.14 的裁决：
    ///   不支持时降级要**被报告**）。
    bool m_DepthStorageImageSupported = false;

    // ── §14.8 任务 22：硬光栅（懒建；`hardRaster` 关闭时全部为空）──
    /// 【设备能力】`surface` 上的静态可判据：mesh shader 可用，且 `DeviceCaps` 的
    ///   `maxMeshWorkGroupInvocations / maxMeshOutputVertices / maxMeshOutputPrimitives`
    ///   容得下本通道的编译期声明（128 / 192 / 64）。为假 ⇒ 永不建 PSO、永不录绘制。
    bool m_HardRasterCapable = false;

    rhi::ShaderBytecode m_HardRasterMS;   // Nanite_HardRaster.mesh.spv
    rhi::ShaderBytecode m_HardRasterFS;   // Nanite_HardRaster.frag.spv

    /// 硬光栅的描述符集布局 + 集合：bindings 0..7 + 12/13/14（与软光栅**同槽位同语义**，
    /// 这样两个 shader 可以 include 同一份 `Nanite_SoftRasterCommon.slang`）。
    /// 【为什么 binding 7 在这里指向**另一个**缓冲】软光栅的 binding 7 是那 16 槽读数缓冲；
    /// 本通道有自己的 4 槽读数缓冲（理由见 `NaniteTypes.h` 的 `kNaniteHardStat*`），
    /// 于是"`u_Stats[...]` 写到哪里"由**描述符指向谁**决定，shader 源码不必分叉。
    rhi::DescriptorSetLayoutHandle m_HardRasterLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_HardRasterSet    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIPipelineState> m_HardRasterPSO;
    /// 本通道的描述符集是否已登记到 bindless 堆（只登记一次；登记后强制一次 Flush）
    bool m_HardBindlessRegistered = false;

    /// 硬光栅读数缓冲（`kNaniteHardStatsCapacity` 条）+ 每帧清零用的常驻 0 源
    std::unique_ptr<rhi::IRHIBuffer> m_HardStats;
    std::unique_ptr<rhi::IRHIBuffer> m_HardStatsZeroSrc;

    /// 上一次录制时记下的硬光栅参数（dump 帧日志用：真实 GPU 读回 + CPU 侧真值对照）
    u32 m_HardLastMaxTriangles = 0u;
    u32 m_HardLastVisibleCapacity = 0u;
    /// 【§14.8 任务 26 / §14.34 第 10 行】上一次**实际推给 mesh 阶段**的那组 push constant 的
    /// CPU 侧真值（= `paramsEff`，包含由绑定侧唯一决定的 `pagesEnabled`）。
    /// 【为什么必须存"推下去的那一份"而不是调用方的入参】回读的用途是核对"CPU 送下去的值 ==
    ///   shader 收到的值"；存调用方入参会在 `pagesEnabled` 这类由录制侧改写的字段上与真实推送值
    ///   分叉，回读判据就会自欺（与软光栅 `m_SoftLastMaxTriangles` 同一口径，只是多存几项）。
    u32 m_HardLastScreenW        = 0u;
    u32 m_HardLastScreenH        = 0u;
    u32 m_HardLastExtentMilli    = 0u;
    u32 m_HardLastInstanceCount  = 0u;
    u32 m_HardLastMaterialCount  = 0u;
    u32 m_HardLastPagesEnabled   = 0u;

    // ── 【§14.8 任务 26】屏幕可视化（懒建；`debugView == 0` 时全部为空）──
    /// 模块自建的 64×32 `R32_UINT` 小目标：**不是** GBuffer 的任何附件 ⇒ 改不动可见画面。
    /// usage = `UnorderedAccess`（compute 原子写）| `TransferSrc`（`CopyTextureToBuffer` 读回）
    ///         | `ShaderResource`（拷完 RHI 无条件还原成 SHADER_READ_ONLY ⇒ 缺这一位会报
    ///         VUID-VkImageMemoryBarrier-oldLayout-01211，与任务 6 的 1×1 目标同一个坑）。 
    std::unique_ptr<rhi::IRHITexture> m_DebugTarget;
    /// 目标 → host 的读回缓冲（`kNaniteDebugViewPixels × 4B`；dump 帧 Map）
    std::unique_ptr<rhi::IRHIBuffer>  m_DebugReadback;
    /// 【模式 4】每簇 BVH 节点深度的 GPU 承载（CPU 侧数组在资源懒建时一次性上传；
    ///   默认档不建）。它只被可视化读，不改任何剔除状态。
    std::unique_ptr<rhi::IRHIBuffer>  m_DebugDepths;

    rhi::ShaderBytecode m_DebugCS;   // Nanite_DebugView.comp.spv
    /// 可视化自己的描述符集布局 + 集合（bindings 0..7；与两条光栅路径**不共用**）
    rhi::DescriptorSetLayoutHandle m_DebugLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_DebugSet    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIPipelineState> m_DebugViewPSO;

    /// 上一次录制用的档位与容量（dump 帧读数行打印；也是"清屏趟"要派发多少组的依据）
    u32 m_DebugLastMode    = 0u;
    u32 m_DebugLastVisibleCapacity = 0u;
};

} // namespace he::render
