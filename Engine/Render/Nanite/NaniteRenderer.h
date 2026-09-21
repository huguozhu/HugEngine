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

    /// 【任务 18】8 个颜色附件 + 深度的**纹理对象**（按 MRT 槽位下标：0=albedo … 7=lightmapKey）。
    /// 任务 18 起模块要①按既有清除值清屏这 8 张 + 深度、②用存储图像写其中 4 张、
    /// ③把深度解析结果写进深度附件 —— 这三件事都只有 `IRHITexture*` 能做（帧图句柄只够排序）。
    /// 【生命周期】同上：归 `GBufferRenderer`；模块只在 pass 内借用。
    rhi::IRHITexture* colorTextures[8] = { nullptr, nullptr, nullptr, nullptr,
                                          nullptr, nullptr, nullptr, nullptr };
    rhi::IRHITexture* depthTexture = nullptr;

    /// 【任务 18】把 8 张颜色 + 深度打包成 `NaniteRaster::GBufferTargets`（模块内部用）
    [[nodiscard]] bool HasFullGBufferTextures() const {
        for (u32 i = 0; i < 8u; ++i) if (!colorTextures[i]) return false;
        return depthTexture != nullptr;
    }
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

    /// 【§14.8 任务 18】把模块自持的软光栅读数组建起来（懒建；资产与 GBuffer 纹理齐了才成功）。
    /// 由 `AddPasses` 在开关开启时调用一次 —— 它**不注册任何 pass**，只保证执行期的资源就绪。
    void EnsureSoftRasterReady(const NaniteGBufferHandles& gb);

    /// 【§14.8 任务 22】把硬光栅（mesh shader 分流）的 PSO/描述符集/读数缓冲懒建起来。
    /// 【门控】`enabled && softRaster && hardRaster && 设备支持`，四个全真才建 —— 因此
    ///   `hardRaster` 默认关时**一个资源都不会创建**（§14.2 不变式 1 的口径），
    ///   `enabled=1` 档的 pass 列表与转储与任务 21 逐位相同。
    /// 【调用点】`AddPasses`，紧跟在 `EnsureSoftRasterReady` 之后（软光栅先建 ⇒ 深度键缓冲一定
    ///   已经在；硬光栅的次序在软光栅之后，见 `NaniteRaster::RecordHardRasterPass` 的推导）。
    void EnsureHardRasterReady(const NaniteGBufferHandles& gb);

    // ============================================================
    // 【§14.8 任务 26 / §14.34 末尾最小范围第 1 条】屏幕可视化的门面接线
    //
    // 【门控】`enabled && IsReady() && debugView != 0`，且三张只读表齐备
    //   （资产簇段 / 簇球 / LOD 元数据 / BVH 深度镜像）。任一不满足：
    //   · 一个 GPU 资源都不建（`NaniteRaster::EnsureDebugViewResources` 连目标都不创建）；
    //   · 不录任何派发（`RecordDebugViewPass` 在渲染器侧就不会被调用）；
    //   · dump 帧一行都不打。
    //   ⇒ 默认档（`debugView == 0`）的日志与转储与今天**逐字/逐位相同**（§14.2 不变式 1）。
    // ============================================================

    /// 懒建可视化资源（不注册 pass；真正的两次派发录在 `Nanite_CullChain3` 的 pass 体内）。
    /// 【为什么放在 public 且由 `AddPasses` 调用】与软/硬光栅同一条既有做法：帧图构建期保证
    ///   执行期资源就绪；帧图**不**为它注册独立 pass（它对零帧图资源的 pass 排序不可依赖）。
    void EnsureDebugViewReady();

    /// 录制可视化（在 `Nanite_CullChain3` 的 pass 体内、剔除链之后调用）。
    void RecordDebugViewPass(rhi::IRHICommandList* cmd);

    /// dump 帧打印**恰好一行**可视化读数（把 64×32 小目标读回 host 后统计）。
    /// 【门控】`debugView == 0` 时直接返回：不 Map、不打一个字符。
    /// 【同步约定】与其它读回相同：只 Map、不等待；调用方必须已 `WaitIdle()`。
    void LogDebugViewReadback();

    /// 【§14.8 任务 18】**让位**的落点：既有 `GB_Clear` 的几何绘制让给模块，本函数只做
    /// "按既有清除值清屏 8×MRT + 深度"（模块自己建 PSO/附件布局、直接写既有 GBuffer 纹理句柄）。
    /// 【调用点】`DeferredPipeline_FrameGraph.cpp` 的 `GB_Clear` pass 体内，由**同一个开关**门控：
    ///   `if (enabled && IsReady() && softRaster) 模块清屏 else 既有 GBufferRenderer::Render(...)`。
    ///   这样既有 pass 的**名字、声明与顺序一个都没变**（判据 ⑥ 的 pass 集合与指纹判据不受影响），
    ///   变的只是那一个 pass 体内"谁写几何"。模块的清屏是**compute 写 8 张颜色目标的 UAV** +
    ///   `ClearDepthStencil`（不依赖 render pass 的 loadOp/缓存行为，见 `Nanite_GBufferClear.comp.slang`）。
    void RecordGBufferClearPass(rhi::IRHICommandList* cmd, const NaniteGBufferHandles& gb);

    /// dump 帧打印**恰好一行**软光栅读数（真实 GPU 读回；字段说明见 `NaniteRaster`）。
    /// 关闭档 / 未就绪 / 未开软光栅时直接返回、不打印（关闭档日志与基线一致）。
    void LogSoftRasterReadback();

    /// 【§14.8 任务 22】dump 帧打印**恰好一行**硬光栅读数（真实 GPU 读回）：
    ///   `[Nanite] hard_raster clusters=<C> prims=<P> pixels=<X> fallback_pixels=<F>
    ///    soft_clusters=<S> soft_pixels=<Y> skipped_big=<B> hard_share_permille=<h>
    ///    soft_share_permille=<s> max_triangles=<T> visible_capacity=<V> mesh_supported=<0|1> pso=<ok|fail>`
    /// 【为什么单起一行而不动 `soft_raster` 行】判据 ⑧a 按字段名 grep 那一行
    ///   （`soft / material_pixels / pixels_written / neutral_material_pixels / skipped_big`），
    ///   改字段名或删字段会让它直接判红。软硬占比由本行与那一行**并列**读出。
    /// 【门控】关闭档 / 未就绪 / 未开软光栅 / `hardRaster=0` 一律不打印（关闭档与"只开 enabled"
    ///   档的日志必须与基线逐字一致）。
    void LogHardRasterReadback();

    /// 【§14.8 任务 23】dump 帧打印**恰好一行**"可见簇的簇大小分布"（真实 GPU 读回）：
    ///   `[Nanite] size_dist buckets=[a,b,c,d,e] total=N clusters=M visible=V
    ///    sum_eq_clusters=1 sum_eq_visible=1 max_triangles=16`
    ///
    /// · `buckets=[…]` = 5 个桶的簇数，区间 `1-4 / 5-8 / 9-16 / 17-32 / 33-64`
    ///   （口径与区间常量见 `NaniteTypes.h` 的"任务 23 五桶"一节）；
    /// · `total` = 五桶之和（即"分类到桶里的可见簇数"）；
    /// · `clusters` = **同一个读数缓冲**里的 `soft + skipped_big`（软光栅分流两侧的合计）；
    /// · `visible` = 可见簇计数缓冲的原子计数（剔除链 Phase 3 的输出，与 `visible_wiring` 行同源）；
    /// · `sum_eq_clusters` / `sum_eq_visible` = **不变式的判定位**（1 = 成立）：
    ///   五桶之和必须等于分流两侧合计（同一次枚举）；受测帧里后者还必须等于 `visible`
    ///   （剔除端写出的每个可见簇都被第 1 趟枚举到 —— 有容量截断或零三角形簇时它会是 0，
    ///   那时读数会把偏差如实标出来，而不是静默）。
    ///
    /// 【为什么单起一行而不追加到 `soft_raster` 行】判据 ⑧a 按字段名 grep 那一行
    ///   （`soft / material_pixels / pixels_written / neutral_material_pixels / skipped_big`），
    ///   动它的字段会让它直接判红；分布是本任务新增的独立口径，另起一行最干净。
    /// 【门控】关闭档 / 未就绪 / 未开软光栅一律不打印（关闭档日志与基线逐字一致）。
    /// 【同步约定】与其它读回相同：只 Map、不等待；调用方必须已 `WaitIdle()`。
    void LogSizeDistReadback();

    /// 【§14.8 任务 26 的第 2 条】读数自检：判定"这一帧的读数是否可信"。
    ///
    /// 【为什么要它（§14.34 表格第 2 行）】"读数必须是真实 GPU 读回"是一条纪律，但纪律本身
    ///   **不是工具**：历史上 `depth_written` 曾硬编码成 1，把一个真 bug 掩盖了整整一个任务；
    ///   任务 24 的第一版也出现过整块读数被覆写（`diag_screenw` 回读成一个小十位数而**不是
    ///   屏幕宽**），而当时 (c)/(d1) 恰好只依赖其中两个字段。⇒ 需要一条**机械**检查。
    ///
    /// 【怎么查——两路，都不需要任何新增输入】
    ///   ① **已知关系式**（恒真，与运行档位无关）：
    ///      `diag_screenw × diag_screenh == depth_key_pixels`（屏幕像素数）、
    ///      `diag_maxtri == SoftLastMaxTriangles()`（push constant 回读必须等于 CPU 送下去的值）、
    ///      `diag_extent_milli > 0`（场景非空时量化尺度不该是 0）。
    ///      这三条是"读数是否真的来自本帧 push constant"的直接证据。
    ///   ② **跨读回的恒定性**：逐个槽记录"是否曾经变化过"。若某槽**非 0 且从未变过**、
    ///      而同一批读回里**确实有别的槽在变**（⇒ 说明帧内容在动），该槽就是"疑似恒真读数"。
    ///      【诚实标注】若本次运行里没有任何槽变化（例如相机固定、只转储一帧），
    ///      则**无法判定**，此时返回 0 并**不报警** —— 宁可漏报也不误报。
    ///
    /// @param s              已读回的软光栅读数缓冲（长度 `kNaniteSoftStatsCapacity`）
    /// @param outDiagOk      出参：1 = ①的三条关系式全部成立；0 = 有任一条不成立
    /// @param outConstSuspect 出参：疑似"恒真读数"的槽数（0 = 未发现或无法判定）
    void SelfCheckSoftStats(const u32 (&s)[kNaniteSoftStatsCapacity],
                            u32& outDiagOk, u32& outConstSuspect);

    /// 【§14.8 任务 23】把"**分流**"与"**帧时**"绑在**同一行**输出（性能读数）：

    ///   `[Nanite] perf max_triangles=16 hard_raster=1 soft_clusters=61 hard_clusters=31587
    ///    soft_pixels=… hard_pixels=… nanite_pass_ms=… nanite_pass_count=2 frame_ms=…
    ///    frame_fps_equiv=… frame_ms_src=gpu_pass_sum`
    ///
    /// 【口径必须一眼看清（这行里的数**都是 GPU 侧**的）】
    /// · `frame_ms` = **`DeferredPipeline::LogFrameBudget()` 自己算出的"各 pass 合计"**，
    ///   由那个函数原样传入（`frameTotalMs`）—— 同一帧、同一数据来源（profiler 的 GPU 时间戳）。
    ///   字段名保留任务书建议的 `frame_ms`，同时用 `frame_ms_src=gpu_pass_sum` **显式标注口径**：
    ///   它是"GPU 各 pass 耗时之和"，**不是**墙钟帧时，也不是 CPU 侧耗时。
    ///   墙钟/CPU 侧的读数在样例既有那一行（"帧率读数（步骤 37）: … 墙钟 X ms ⇒ Y fps；
    ///   CPU 侧 Z ms"）；两者**不要混读**（本样例实测就是 CPU 受限：GPU 合计约 42 ms 而墙钟约 146 ms）。
    ///   **为什么必须这样取**：若本行自己再测一遍帧时，同一次运行里就会出现两个互相打架的数字，
    ///   "分流对帧时的影响"也就无从判定。
    /// · `nanite_pass_ms` = profiler 里**名字以 `Nanite` 开头**的 pass 的 GPU 耗时之和
    ///   （硬光栅录在 `Nanite_CullChain3` 的 pass 体内 ⇒ 它的成本就体现在这个数的变化里）；
    /// · `nanite_pass_count` = 参与求和的 pass 数（受测档应为 2：`Nanite_Cull` + `Nanite_CullChain3`）。
    ///
    /// 【为什么叫 `frame_fps_equiv` 而不是 `frame_fps_cap`】它就是 `1000 / frame_ms`，即
    ///   "**由 GPU pass 合计推出的等效帧率**"，不是帧率上限、也不是被 cap 的帧率。
    ///   本样例实测它与真实帧率差一个数量级（GPU 合计 11.6 ms ⇒ 86 fps，而墙钟 140 ms ⇒ 7.1 fps，
    ///   因为整帧 CPU 受限）—— 名字里必须带 `equiv` 才不会被误读成"帧率达标"。
    ///
    /// 【开关：`HE_CPU_PASSES`】**默认关闭 ⇒ 不打印、也不做任何读回**（§14.2 不变式 1 的
    ///   "关闭时不产生新开销"口径；`HE_CPU_PASSES` 是仓库既有的 CPU 步骤计时开关，
    ///   `DeferredPipeline_FrameGraph.cpp` 与 `RenderGraph.cpp` 已在用它，本行复用同一个开关，
    ///   不另造一套 plumbing）。
    /// 【调用点】`DeferredPipeline::LogFrameBudget()` 的末尾（它每 120 帧打印一次整帧预算）。
    /// @param frameTotalMs   `LogFrameBudget` 算出的整帧合计（ms）
    /// @param nanitePassMs   profiler 里 Nanite 前缀 pass 的 GPU 耗时合计（ms）
    /// @param nanitePassCount 参与求和的 Nanite pass 数
    void LogPerfReadback(float frameTotalMs, float nanitePassMs, u32 nanitePassCount);

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

    /// 【§14.8 任务 25】dump 帧打印**恰好一行** Material Bin 的读数：
    ///   `[Nanite] material_bin descriptor_switches=<D> material_switches=<M>
    ///    material_switches_bin=<B> order_src=gpu_visible_cluster_buffer visible_refs=<R>
    ///    bin_clusters=<P> material_switches_asset_order=<A>
    ///    clusters_per_material=[distinct=<d> max=<x> min=<n> mean=<m>]`
    ///
    /// 【三个数的口径（每个都写清"测的是什么"，否则读数无法核对）】
    /// · `descriptor_switches` = 本帧**材质切换导致的描述符集切换次数**（验收明文要求的那个数）。
    ///   **它恒为 0～1 是结构决定的，不是"没测到"**：本模块的软/硬光栅各自只有一对描述符集
    ///   （`NaniteRaster`），材质是**索引进一个 SSBO**（`u_Materials[materialID]`）+ **bindless**
    ///   纹理数组（`u_MaterialTextures[]`）取的，帧内不重绑 ⇒ 真实发生的切换只有"进入软光栅趟
    ///   （1 次）"与"进入硬光栅趟（0/1 次）"。§5.4 想减少的"每材质一个描述符集"那种切换，
    ///   在本仓库的延迟路径里**从未存在**（论据见 `docs/已实现功能/Nanite设计与实现.md` §14.33 ①②）。
    /// · `material_switches` = **相邻处理的簇换材质的次数**（局部性代理）。
    ///   **顺序口径 = GPU 可见簇列表缓冲的槽位顺序**（`NaniteCull::GetVisibleClusterBuffer`，
    ///   Phase 3 写出的真实列表；软光栅第 1 趟与硬光栅 mesh 工作组都按这个下标顺序枚举）
    ///   ⇒ 测的就是 GPU 真实的处理顺序。`visible_refs` 是本行实际统计到的条数（分母）。
    ///   【如实标注一处细节】该列表由 Phase 3 用原子槽位压缩写出，槽位顺序不保证等于 CPU 参考
    ///   遍历顺序；但"GPU 真正按什么顺序处理"正是这个槽位顺序，故本读数**不**改用 CPU 列表。
    /// · `material_switches_bin` = **若按 bin 顺序遍历**（同一批可见簇按"按材质分组"重排）时的同一个
    ///   数 —— 它与 `material_switches` 的差就是"按材质分组"这项优化的收益证据。
    ///   `bin_clusters` = 参与该对照的簇数（正常情况下等于 `visible_refs`）。
    /// · `material_switches_asset_order` = **资产自然顺序**（簇下标 `0..N-1`）下的同一个数。
    ///   【为什么多这一个】当前顺序随相机变，单看它回答不了"bin 与不分组差多少"这种与相机无关的
    ///   问题；资产自然顺序是**确定的**，且正是 `Tests/TestNaniteMaterialBin.cpp` 钉住的口径 ⇒
    ///   日志与单测可以互相对账（该用例实测：自然顺序远高于 bin 顺序，且 bin 顺序 = 材质数 - 1）。
    /// · `clusters_per_material=[…]` = **资产**的逐材质簇数分布摘要（四个数：出现的材质数 /
    ///   每材质最大 / 最小 / 平均簇数），不打印上百个桶。
    ///
    /// 【本函数**不做**的事（本任务的"明确不做"，理由必须写清）】它**不**把光栅的遍历顺序改成
    ///   bin 顺序。软光栅在**深度键平局**时像素由 **UAV 写入顺序**决定（§14.31 ⑩ 实测：模块接管档
    ///   两次相同运行并非逐位可复现，`lightmapkey.page` 平均差 18.8）⇒ 在平局确定性修好之前改
    ///   遍历顺序会**改变画面**，属"先修根因再谈优化"。bin 因此只是**只读的收益证据**。
    ///
    /// 【门控】`materialBin=false`（默认）时直接返回：不 Map 任何缓冲、不打一个字符
    ///   （关闭档的日志与转储必须逐字/逐位不变）。
    /// 【同步约定】与其它读回相同：只 Map、不等待；调用方必须已 `WaitIdle()`。
    /// 【资源】本行**不新增任何 GPU 资源**：bin 是 CPU 侧的 `u32[]`（上传期一次），
    ///   其余三个读数全部来自既有缓冲（可见簇计数缓冲 / 可见簇列表缓冲 / GPU 相位计数）。
    void LogMaterialBinReadback();

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

    // ============================================================
    // 【§14.8 任务 24】LOD 流式（反馈 + 页池）的门面
    //
    // 【门控】生效条件是 `enabled && softRaster && streaming`（§14.32 ⑧）。三者任一不满足：
    //   · 不建任何页池资源（`m_Stream` 仍是"只记住设备"的空壳）；
    //   · 不产生每帧开销（`StepStreaming` 里一次 `IsReady()` 判空就返回，连同步都不做）；
    //   · 默认档（`streaming=0`）**不打印** `stream` 行 ⇒ 关闭档日志逐字不变。
    // ============================================================

    /// **懒建**页池/页表/反馈环（只做一次；资产留存的 CPU 副本是它的数据源）。
    /// 【为什么在帧图构建期调用】`AddPasses` 每帧都会走到，而真正的资源只在**第一次**
    ///   有效调用时创建（`m_StreamSetupTried` 门闩）；这也保证"关闭档一个资源都不建"。
    /// 【退化】`Setup` 失败时原因记在 `m_Stream.Reason()`（`pool_zero_slots` / `plan_failed` /
    ///   `page_straddle` / `no_asset` / `resource_failed`），由 `LogStreamReadback` 如实打印。
    void EnsureStreamReady();

    /// **每帧步进**：读回延迟反馈 → 合并去重 → LRU 淘汰 → 限流上传 → 重写页表。
    /// 【同步】内部在帧图构建期做一次 `WaitIdle()`（页表与页池是 CPU 写、GPU 读，只有
    ///   "没有在飞命令"时才无竞态）；未就绪时**一次都不做**（默认档零开销）。
    void StepStreaming();

    /// dump 帧打印**恰好一行**流式读数：
    ///   `[Nanite] stream pages_total=P resident=R pool=K uploads_this_frame=U evicted=E
    ///    page_misses=M pages_requested=Q stream=on/off reason=<...>`
    ///
    /// 【字段口径（每一个都是可核对的确定性量）】
    /// · `pages_total`  = 页数（由资产自身推出的纯函数：`ceil(共享内容份数 / K)`）；
    /// · `resident`     = CPU 侧认为驻留的页数；`gpu_resident` = **页表缓冲里真的被标成驻留的条数**
    ///   （Map 读回 ⇒ 它同时证明"CPU 的账"和"GPU 看到的内容"一致）；
    /// · `pool`         = 页池槽数（cfg）；`uploads_this_frame` ≤ 每帧上限；
    /// · `evicted`      = 累计淘汰页数；
    /// · `page_misses`  = **真实 GPU 原子计数**：本帧因页未驻留被跳过的**可见簇数**；
    /// · `pages_requested` = CPU 本帧从延迟反馈里读出的请求条数（含重复页）；
    /// · `pages_requested_total` / `requests_total` = 累计（非空洞守卫：池足够大时**必然 > 0**，
    ///   因为开局的每一页都要先被请求一次才会驻留）；
    /// · `stream=on/off reason=<...>` = 生效状态与**退化原因**（不静默）。
    ///
    /// 【门控】`enabled && streaming` 才打印（默认档与关闭档的日志必须与基线逐字一致）。
    /// 【同步约定】与其它读回相同：只 Map、不等待；调用方必须已 `WaitIdle()`。
    void LogStreamReadback();

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

    /// 【§14.8 任务 18】软光栅两趟的 push constant（`AddPasses` 每帧填一次，录制期按值使用）。
    /// 【为什么是成员而不是 lambda 捕获】参数要在**帧图构建期**算（view-proj 与屏幕尺寸），
    ///   而在**执行期**推给 GPU；与任务 15 的 `m_ChainParams` 同一套做法（帧图是单线程构建的）。
    NaniteSoftRasterParams m_SoftParams{};

    /// 【§14.8 任务 26】可视化的 push constant（`AddPasses` 每帧填一次；`debugView == 0` 时
    ///   它不会被任何录制路径读到 —— 默认档零开销，只是结构体里多 96B 的成员）。
    /// 【为什么 `vpRows` 不共用 `m_SoftParams` 的那 16 个 float】两份是**同样的填法**
    ///   （同一个双重循环、同一份比特），但各自的 push constant 是独立的结构；让可视化依赖
    ///   "软光栅恰好也在跑"会把两个档位绑死（可视化在 `softRaster=0` 档也该能用）。
    NaniteDebugViewParams m_DebugParams{};

    /// 【§14.8 任务 18】资产的位置量化尺度（整网格最大轴长）。由 `EnsureAssetUploaded` 从
    ///   打包读数里取（与 DAG 哈希/顶点词同一个函数算出来的那份），资产未入库时为 0
    ///   ⇒ 软光栅会解出退化位置，故执行期还有"资产是否入库"的门控。
    float m_MeshMaxExtent = 0.0f;

    /// 【§14.8 任务 19】材质段条数（= 逐源网格一条；`AddPasses` 填进 push constant 的
    ///   `materialCount`，执行期由 shader 用来判"材质下标是否越界"）。资产未入库时为 0。
    u32 m_MaterialCount = 0u;
    /// 【§14.8 任务 19】三角形跨越 ≥2 个源网格的簇数（映射读数，如实报告）
    u32 m_MultiMeshClusters = 0u;

    // ── 【§14.8 任务 24】LOD 流式的状态 ──
    /// 页池 + 页表 + 反馈环 + 驻留管理的宿主（默认档是空壳：一个缓冲都不建）
    NaniteStream m_Stream;
    /// "只尝试建一次"的门闩（`Initialize`/`Shutdown` 复位）。**只在资产 CPU 副本已就绪时才置位**
    /// —— 否则第一帧资产还没入库就会把一次 `no_asset` 当成永久结论。
    bool m_StreamSetupTried = false;
    /// 流式自己的帧号（从流式生效的第一帧开始数）。
    /// 【为什么不复用引擎的帧计数】反馈环的槽位 = 帧号 % 延迟，"延迟恰好等于常量"这条口径
    ///   依赖一个**只在本模块内线性递增**的计数；复用引擎计数会把它的语义绑死在引擎实现上。
    u32 m_FrameIndex = 0u;
    /// 资产的簇出现总数（页映射的索引空间上界；流式开启档给着色器做越界收口用）
    u32 m_AssetClusterCount = 0u;

    // ── 【§14.8 任务 26 第 2 条】读数自检的跨读回状态 ──
    /// 自检被调用过几次（0 = 还没有基线，第一次只记录不下结论）
    u32 m_StatSelfCheckCalls = 0u;
    /// 上一次的读数快照（用于判定"这一帧有没有东西在变"）
    u32 m_StatPrev[kNaniteSoftStatsCapacity] = {};
    /// 每个槽"是否曾经变化过"（1 = 变过）。**只增不减**：一旦变过就不再是"恒真"嫌疑。
    u8  m_StatEverChanged[kNaniteSoftStatsCapacity] = {};

    /// 开关与档位的唯一真值（默认 `enabled = false` ⇒ §14.2 不变式 1）
    NaniteSettings m_Settings;

    // 模块内部各段（§14.3 的六个文件各司其职，本类只做门面与生命周期转发）
    NaniteScene  m_Scene;
    NaniteUpload m_Upload;
    NaniteCull   m_Cull;
    NaniteRaster m_Raster;
};

} // namespace he::render
