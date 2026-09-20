#pragma once

// ============================================================
// Nanite/NaniteRenderer.h — Nanite 模块的唯一门面（生命周期 + 帧图接入）
//
// 【本文件由 §14.8 任务 1 建立骨架，内容由任务 3/4/6 依次填充】
//   任务 1：骨架 + 独立开关（唯一注册的 pass 是 `Nanite_Noop`）。
//   任务 3：把占位 pass 换成「计数 → 间接绘制」链的两个 pass：
//     · `Nanite_Cull`   —— compute 写间接命令 + 计数（`NaniteCull`）
//     · `Nanite_Raster` —— `DrawIndexedIndirectCount` 消费计数，写模块自建的 1×1 R8 目标
//   `Nanite_Noop` 同时被移除（它的历史作用只是验证门控）。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法）；不得依赖 `MeshBatcher` 的运行时状态
//   —— 它只当**一次性输入**。本类只依赖 `RenderGraph` 与 RHI 的公开接口，
//   不 include 任何 GI/Lumen/GPUCulling 头。
//
// 【公共面】§14.3：模块公共面只有三个 —— 本类、`NaniteSettings`、`NaniteTypes.h` 的 POD。
//   `NaniteScene / NaniteUpload / NaniteCull / NaniteRaster` 是模块内部实现，
//   外部（含 `DeferredPipeline`）只通过本类使用它们。
// ============================================================

#include "Nanite/NaniteCull.h"
#include "Nanite/NaniteRaster.h"
#include "Nanite/NaniteScene.h"
#include "Nanite/NaniteSettings.h"
#include "Nanite/NaniteUpload.h"

#include "RHI/RHI.h"
#include "RenderGraph.h"

#include <functional>   // 【任务 15】NaniteHiZSource 的两个回调

namespace he::render {

class MeshBatcher;   // 【任务 12】只作**一次性输入**的类型：头文件不 include，避免把它的
                     // 依赖（Scene/GPUScene/RHI）带进模块门面；`.cpp` 里才 include 它的头。

struct CameraData;   // 【任务 13】渲染相机（`Pipeline/Camera.h`）。`AddPasses` 只用它的 const 引用
                     // 取 view-proj 与相机位置，故头文件里前置声明即可，不牵入该头的实现。

/// 帧图接入所需的 GBuffer 句柄组（与 `GBufferRenderer::Handles` 同构）
///
/// 任务 3 只用到 `depth` / `worldPos`（复刻 `GB_Clear` 的那组 WAW 声明，§14.5 第一条硬约束）；
/// 其余字段为任务 4/5 的光栅与材质解析预留 —— 届时模块**自己**建 PSO/附件布局、
/// 直接写这些既有句柄，从而不需要给 `GBufferRenderer` 加 `Mode::Nanite`（§14.5）。
struct NaniteGBufferHandles {
    ResourceHandle albedo      = kInvalidHandle;
    ResourceHandle normal      = kInvalidHandle;
    ResourceHandle emissive    = kInvalidHandle;
    ResourceHandle velocity    = kInvalidHandle;
    ResourceHandle worldPos    = kInvalidHandle;
    ResourceHandle disneyA     = kInvalidHandle;
    ResourceHandle disneyB     = kInvalidHandle;
    ResourceHandle lightmapKey = kInvalidHandle;
    ResourceHandle depth       = kInvalidHandle;

    /// 【任务 4 新增】albedo 的**纹理对象**本身（不只是帧图句柄）。
    /// 任务 4 的 `Nanite_TestWrite` 要把它绑成存储图像（UAV）并从它取真实分辨率，
    /// 这两件事都只有 `IRHITexture*` 能做；句柄只够帧图排序。
    /// 【生命周期】纹理归 `GBufferRenderer` 所有；模块只在一个 pass 内借用（不持有）。
    /// 前一处挂钩（`AddPasses`）不用它，故默认 nullptr。
    rhi::IRHITexture* albedoTexture = nullptr;
};

/// 【§14.8 任务 15】Hi-Z 金字塔的来源（由 `DeferredPipeline` 在帧图构建期提供）
///
/// 【为什么是回调而不是直接传纹理指针】
///   ① 第 1 帧的帧图构建期，`GPUCulling` 的 Hi-Z 纹理还没被创建（它是在 `GPU_Cull` pass 执行期
///      由 `SetDepthTexture` 建的）⇒ 纹理指针必须在 **pass 执行期**取；
///   ② 窗口尺寸变化会**重建**纹理 ⇒ 构建期捕获的裸指针会悬空；
///   ③ §14.3 禁止模块 include `GPUCulling.h`。
/// 【金字塔由谁构建（本次实测后的裁决）】模块自己构建，但**复用 GPUCulling 的纹理资源**与它那套
///   下采样口径（`R32_FLOAT` / ≤8 层 / 层 L = 2^L 足迹最小深度 / mip0 不写）。
///   【为什么不是直接调 `GPUCulling::BuildHiZPyramid`】实测该函数在本引擎里**不可能**构建出正确
///   金字塔：它在循环里逐 mip 更新**同一个**描述符集，而本引擎的 GPU 在**执行期**读取描述符、
///   最后一次主机写对整段命令缓冲生效（已用对照实验钉死）⇒ 7 次派发全部用最后一个状态，
///   结果整张金字塔全 0。`GPUCulling.*` 在本次改动面之外，故模块侧按同一口径自建。
///   完整证据、上游修法与影响面见任务 15 实施记录。
struct NaniteHiZSource {
    /// 取当前帧的 Hi-Z 纹理（金字塔资源本身；执行期调用；nullptr ⇒ 本帧退化为"Hi-Z 关闭"）
    std::function<rhi::IRHITexture*()> texture;
    /// 取当前帧的深度纹理（金字塔的输入；执行期调用）
    std::function<rhi::IRHITexture*()> depth;

    [[nodiscard]] bool valid() const { return texture && depth; }
};

/// Nanite 模块门面：资源生命周期 + 帧图接入 + 耗时读数/诊断（后两者是后续任务）
class NaniteRenderer {
public:
    NaniteRenderer() = default;
    ~NaniteRenderer() = default;

    NaniteRenderer(const NaniteRenderer&) = delete;
    NaniteRenderer& operator=(const NaniteRenderer&) = delete;

    /// 建立骨架并读入配置层的启动默认值。
    /// 【说明】CVar `r.Nanite.Enable` 只在这里读一次，作为**启动默认**；
    /// 运行期真值是 `m_Settings`（cfg 与面板都写它），三层不会互相覆盖。
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);

    /// 释放模块自持资源。**不重置开关真值** —— 样例的配置回写发生在
    /// `DeferredPipeline::Shutdown()` 之后，清掉真值会让 cfg 往返丢键。
    void Shutdown();

    /// 视口变化（GBuffer 尺寸变了，软光栅/剔除的目标随之变化）
    void Resize(u32 width, u32 height);

    /// 骨架就绪（任务 1：`Initialize` 成功即 true）。
    /// 帧图门控要用它：若这里恒为 false，`nanite_enable=1` 时不会出现 `Nanite_Noop`。
    [[nodiscard]] bool IsReady() const { return m_Ready; }

    // ── 开档与档位（§14.4：唯一真值在本类，外部只拿 const 引用读）──
    [[nodiscard]] const NaniteSettings& GetSettings() const { return m_Settings; }
    void SetSettings(const NaniteSettings& settings) { m_Settings = settings; }

    /// 帧图接入点（在 `DeferredPipeline_FrameGraph.cpp` 的 GBuffer 段被调用）。
    /// 【门控只有一处】`DeferredPipeline_FrameGraph.cpp` 里的
    /// `if (m_Nanite.GetSettings().enabled && m_Nanite.IsReady())`。
    /// 开启时注册两个 pass：`Nanite_Cull` + `Nanite_Raster`（原序 12 个 pass 一个不动）；
    /// 【§14.8 任务 6】`meshTest` 为真时**再追加**一个 `Nanite_MeshTest`（mesh PSO 通道）。
    /// 【§14.8 任务 15 的改动】`Nanite_InstanceCull` 与 `Nanite_ClusterBVH` 两个 pass **不再**在
    ///   这里注册 —— 它们被合并成**一个** `Nanite_CullChain3` pass（Phase 1 → Phase 2/3 同一条链），
    ///   并搬到 `AddPostGBufferPasses`（原因：它的 Phase 2 要采样**本帧** GBuffer 深度建出来的
    ///   Hi-Z 金字塔，因此必须排在 `GB_Clear` 之后；注册在本函数后面会被帧图排到 GB_Clear 之前）。
    ///
    /// 【§14.8 任务 13/15 的 camera 参数】实例剔除需要世界空间视锥与相机位置：
    ///   `NaniteCull::SetCullChainFrame` 由 view-proj 提取 6 平面、以相机位置为基准生成
    ///   合成实例网格，并用相机的 fov + 屏幕高度算 Phase 3 的**像素焦距**。
    ///   【为什么从调用方传入】相机的唯一持有者是 `DeferredPipeline`；模块不自造也没有别处可取。
    void AddPasses(RenderGraph& rg, const NaniteGBufferHandles& gb, const CameraData& camera);

    /// 【§14.8 任务 4：GBuffer 之后的后置挂钩】在 **GBuffer 几何段结束之后、任何读取 GBuffer
    /// 的 pass（SSAO/Lighting 等）之前**调用第二处注册点：
    ///   · 为什么必须有这一处：`AddPasses` 的位置在既有 `GB_Clear` **之前**，模块的写入会被
    ///     `GB_Clear` 覆盖，无法被同帧的 Lighting 读到；任务 4 的验收（"compute 写 GBuffer 且
    ///     同帧被 Lighting 读到"）只能在 GBuffer 之后注册才能成立。
    ///   · 开关守卫与 `AddPasses` 是**同一个真值**（`NaniteSettings::enabled` + `IsReady()`），
    ///     不是新门控；`nanite_test_write` / 三阶段链只是模块内部的第二个条件。
    ///   · 关闭档 / 未就绪 ⇒ 一个 pass 都不注册（§14.2 不变式 1）。
    ///   · 注意：`Nanite_TestWrite` 写的是 `gbAlbedo` 的 UAV，因此**不能**声明 `gbDepth/gbWorldPos`
    ///     的那组 WAW —— `GB_Clear` 已经在前面写过它们，再声明只会多出一条无意义的依赖。
    ///
    /// 【§14.8 任务 15/16】`Nanite_CullChain3`：三阶段簇剔除链（Phase 1 实例剔除 + 掩码 →
    ///   Phase 2 构建 Hi-Z 并做视锥/遮挡剔除 → Phase 3 LOD 选择）在这里注册：
    ///   · 声明 `reads = {gbDepth}`：这是**真的**在读（Hi-Z 金字塔由本帧深度下采样而来），
    ///     它同时给帧图一条把本 pass 定序在 `GB_Clear` 之后的 RAW 依赖；
    ///   · 内部三段的相对顺序**不靠帧图**，而是靠同一 pass 体内命令缓冲的屏障（见 `NaniteCull`）。
    ///   · `hiz` 为空（默认）⇒ 该 pass 照常注册，但 Hi-Z 遮挡测试关闭（退化为"不遮挡"）。
    ///   · 【任务 16】当可见链是**绘制来源**时，本 pass 在算完之后**紧接着录制间接绘制**
    ///     （`NaniteRaster::RecordRasterPass`）—— 见下方"绘制录在哪个 pass 内"的说明。
    /// 【将来】任务 26 的调试可视化落点也在这里（GBuffer 之后的可视化叠加）。
    void AddPostGBufferPasses(RenderGraph& rg, const NaniteGBufferHandles& gb,
                              const NaniteHiZSource& hiz = NaniteHiZSource{});

    // ============================================================
    // §14.8 任务 12：资产构建 + 一次性上传 + 读数校验的门闩
    // ============================================================

    /// 【§14.8 任务 12】把 `MeshBatcher` 的**合并几何**当作一次性输入，做一次
    /// "资产构建（CPU 字节镜像）→ GPU 上传 → 真实读回校验"，并打印恰好一行
    ///   `[Nanite] upload_bytes=<N> readback_match=<0|1> mismatch_bytes=<M> clusters=… lod_levels=…`
    ///
    /// 【门控与次数：这是本任务"只做一次"的实现点】
    ///   · 开关关闭 / 模块未就绪 ⇒ **直接返回**：一个 GPU 资源都不建、一行日志都不打
    ///     （§14.2 不变式 1，关闭档与基线逐位一致）；
    ///   · 已经做过一次 ⇒ 直接返回（`m_AssetUploaded` 门闩）。
    /// 【几何来源】只读**一次** `batcher.GetMergedVertices()/GetMergedIndices()`
    ///   （`MeshBatcher.h:60-61` 已公开的 const getter，本任务**没有**改 `MeshBatcher`），
    ///   随后转成扁平 SoA 交给 RHI-free 的 `BuildNaniteAssetFromGeometry()`。
    ///   `MeshBatcher` 只以**参数**形式出现一次，模块不持有它的指针、不在每帧回读它的表
    ///   （§14.3 的依赖禁令；与 `LumenSDF::Step(cmd, batcher)` 同款口径）。
    /// 【调用点】`DeferredPipeline_FrameGraph.cpp`，在"合并几何已构建"之后、注册模块 pass 之前。
    ///   为什么不在样例里触发：合并几何的唯一持有者是 `DeferredPipeline`，样例侧拿不到它，
    ///   硬加一层访问器只会扩大接触面。
    /// 【已知口径】本任务把**整份**合并几何当成"一个 `.nanite` 资产"（不做逐网格资产拆分、
    ///   不施加每物体变换、不解析材质）；这些属 §14.4 的每网格资产路径与任务 19 的材质解析。
    void EnsureAssetUploaded(const MeshBatcher& batcher);

    /// dump 帧（`HE_DUMP_GI_FRAME`）打印**恰好一行**真实 GPU 读回：
    ///   `[Nanite] fake_clusters=<N> count_buffer=<X> indirect_cmds=<Y> rasterized_clusters=<Z>`
    ///
    /// X = 计数缓冲的值（GPU 原子累加的命令条数）；Y = 间接命令缓冲里**实际被写过**的
    /// 命令条数（读回时按"非哨兵且字段合法"统计）；Z = 绘制端"每个绘制恰好一次"的原子计数。
    ///
    /// 【§14.8 任务 16 的门控改动】这一行只在**假簇链是绘制来源**时打印（`nanite_fake_chain=1`，
    ///   或可见链尚未就绪的退化帧）。默认档（走可见簇列表）由 `LogVisibleWiringReadback()`
    ///   打印那一行 —— 否则同一帧会有两条互相矛盾的"画了多少"读数。
    void LogFakePipelineReadback();

    /// 【§14.8 任务 16】dump 帧打印**恰好一行**"可见簇 → 间接绘制"的接线读数：
    ///   `[Nanite] visible_wiring visible=<V> indirect_count=<C> draws=<D> rasterized=<R>
    ///    empty_draws=<E> mismatch=<M> src=<visible|fake> truncated=<T> max_draws=<X>
    ///    cpu_cmds=<P> placeholder_indices=<Q>`
    ///
    /// 四个核心数**全部来自真实 GPU 读回**，且来源彼此独立：
    /// · `visible`       = 可见簇计数缓冲（剔除端 Phase 3 的原子计数）—— "应该画多少条"；
    /// · `indirect_count`= 间接命令缓冲 `[0, V)` 里**字段合法且与 CPU 参考逐字段一致**的条数
    ///                     （CPU 逐字节读回 GPU 内存后核验）—— "命令缓冲里真的有这么多条"；
    /// · `draws`         = 绘制计数缓冲（= 喂给 `DrawIndexedIndirectCount` 的 count；剔除端只在
    ///                     "真的写了命令"时 +1）—— "间接参数条数"；
    /// · `rasterized`    = 绘制端片元的原子计数（`SV_PrimitiveID == 0`，每个绘制恰好一次）
    ///                     —— "GPU 真的执行了这么多次绘制"。
    /// 【验收判据】正常档 `V == C == D == R`、`empty_draws = 0`、`mismatch = 0`。
    ///   `empty_draws` = V 里"画了但没产生任何片元"的条数（命令有效但几何被丢弃）；
    ///   `mismatch` = 逐条字段不一致数 + |V−C| + |V−D| + |D−R|（任一非 0 都说明接线有问题）。
    ///   `truncated` = 因绘制容量不足而未写命令的簇数（`nanite_draw_capacity` 的截断自证）；
    ///   `cpu_cmds` = CPU 参考打包出的命令条数（与 `indirect_count` 同口径，供交叉核对）。
    ///
    /// 【同步约定】与 `LogFakePipelineReadback` 相同：只做 Map 读回、不做等待；调用方必须已
    /// `WaitIdle()`。关闭档 / 未就绪时直接返回、不打印 —— 保证关闭档日志与基线一致。
    void LogVisibleWiringReadback();

    /// 【§14.8 任务 15】dump 帧打印**恰好一行**三阶段剔除的 GPU/CPU 逐项对照：
    ///   `[Nanite] cull3 phase1=<a> phase2=<b> phase3=<c> hiz=<on|off> gpu_clusters=<C>
    ///    cpu_clusters=<C> mismatch=<M> lod=[…] extra_gpu=… occluded=… frustum=… …`
    ///
    /// · `phase1` = GPU 读回的**可见实例数**（Phase 1 的输出；与 CPU 参考升序集合逐项一致）；
    /// · `phase2` = GPU 读回的"通过视锥 + Hi-Z"的簇引用数（Phase 2 的输出）；
    /// · `phase3` = GPU 读回的"再经 Phase 3 LOD 选择"后的簇引用数（= `gpu_clusters`）；
    /// · `hiz` = Hi-Z 遮挡测试**是否真的生效**（`on` 要求金字塔层数 ≥ 2 且外部纹理已绑定）；
    /// · `cpu_clusters` = CPU 参考的最终可见簇数（**Hi-Z 恒关闭**：CPU 拿不到金字塔的逐 texel
    ///   内容，理由见 `NaniteCull::RunCullChainCPUReference`）；
    /// · `mismatch` = 两个可见簇**集合**的对称差大小（排序后做真正的集合差，不是只比计数）；
    /// · `extra_gpu` = `gpu \ cpu`（**Hi-Z 只能少不能多** ⇒ 这一项非 0 就是真 bug，不是"可解释差异"）；
    /// · `occluded` / `frustum` = 被 Hi-Z 剔除 / 通过视锥的簇引用数（Phase 2 的两个中间读数）；
    /// · `lod=[…]` = Phase 3 的**选中级别分布**（GPU 读回；8 个槽对应 LOD 0..7）；
    /// · `occl_mip=[…]` = Hi-Z 打开时"CPU 可见但 GPU 判遮挡"的簇的**选层分布**（CPU 侧按同一公式
    ///   复算）—— 它把"两档差异"量化到"这些簇恰好是投影盒较大、落在金字塔较深层的那批"。
    ///
    /// 【同步约定】与 `LogFakePipelineReadback` 相同：只做 Map 读回、不做等待；调用方必须已
    /// `WaitIdle()`。关闭档 / 未就绪时直接返回、不打印 —— 保证关闭档日志与基线一致。
    void LogCull3Readback();

    /// 【§14.8 任务 6】dump 帧打印**恰好一行** mesh 通道的真实 GPU 读回：
    ///   `[Nanite] mesh_pso=<ok|fail> meshlet_outputs=<n> target_max=<v>`
    ///
    /// n = mesh 通道片元的原子计数（本帧被光栅化的 mesh 图元数；mesh shader 输出 2 个三角形
    /// 且目标是 1×1 ⇒ 期望 2）；v = 模块自建的 1×1 R8 目标的读回值（写 1.0 ⇒ 期望 255）。
    /// 两个数**互相独立**（一个来自缓冲、一个来自纹理拷贝），任一 > 0 都说明"输出了非空画面"。
    ///
    /// 【同步约定】与 `LogFakePipelineReadback` 相同：只做 Map 读回，不做等待；调用方必须已
    /// `WaitIdle()`。`meshTest=false`（默认）时直接返回、不打印 —— 保证关闭档与开启档
    /// （不勾 mesh 自证时）的日志与基线一致。
    void LogMeshTestReadback();

    // ── 模块内部各段（任务 1 只有生命周期桩；外部不得越过本类直接驱动它们）──
    [[nodiscard]] NaniteScene&  GetScene()  { return m_Scene; }
    [[nodiscard]] NaniteUpload& GetUpload() { return m_Upload; }
    [[nodiscard]] NaniteCull&   GetCull()   { return m_Cull; }
    [[nodiscard]] NaniteRaster& GetRaster() { return m_Raster; }

    [[nodiscard]] u32 GetWidth()  const { return m_Width; }
    [[nodiscard]] u32 GetHeight() const { return m_Height; }

private:
    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width  = 0;
    u32 m_Height = 0;
    bool m_Ready = false;   ///< "骨架就绪"（任务 1 的语义，见 IsReady 的说明）

    /// 【§14.8 任务 12】资产"只上传一次"的门闩（`Initialize` 时复位 ⇒ 重建后可再来一次）。
    /// 它的存在保证 `EnsureAssetUploaded` 即使在帧循环里被反复调用，也只做一次 GPU 上传 + 读回。
    bool m_AssetUploaded = false;

    /// 【§14.8 任务 16】本帧的**绘制来源**是否退化到假簇链（`AddPasses` 每帧算一次）。
    ///
    /// 【为什么需要这个标志】两条链都要"谁来画"唯一：可见链的绘制录在 `Nanite_CullChain3`
    ///   体内、假簇链的绘制录在 `Nanite_Cull` 体内（都紧跟在自己的 compute 之后，顺序由命令
    ///   缓冲的屏障给出）；若两条都画就会把同一批读数加两遍，若都不画则一个簇都不画。
    /// 【退化条件】`nanite_fake_chain=1`（显式自证）**或** 可见链尚不可用
    ///   （资产/BVH 还没入库 —— 那时 Phase 2/3 根本不派发，可见链一条命令都产不出来）。
    ///   注意"实例数为 0"**不算**退化：那正是"零可见簇 ⇒ 零绘制"这条边界，必须走可见链
    ///   （走假簇链会画出 6 条，把边界验收掩盖掉）。
    bool m_DrawFromFakeChain = false;

    /// 开关与档位的唯一真值（默认 `enabled = false` ⇒ §14.2 不变式 1）
    NaniteSettings m_Settings;

    // 模块内部各段（§14.3 的六个文件各司其职，本类只做门面与生命周期转发）
    NaniteScene  m_Scene;
    NaniteUpload m_Upload;
    NaniteCull   m_Cull;
    NaniteRaster m_Raster;
};

} // namespace he::render
