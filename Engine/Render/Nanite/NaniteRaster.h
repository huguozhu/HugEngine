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

#include "Nanite/NaniteTypes.h"   // 【任务 18】NaniteSoftRasterParams / 读数槽位 / 深度键编码

#include <memory>

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

    /// 软光栅是否就绪（资源 + PSO 都建起来了）
    [[nodiscard]] bool IsSoftRasterReady() const { return m_SoftColorPSO != nullptr; }

    /// 【任务 18】懒建软光栅资源（两趟 compute PSO + 深度解析 PSO + 三套描述符集布局）。
    /// 【为什么放在 public】`NaniteRenderer`（门面）在**帧图构建期**调用它：帧图不注册这个 pass，
    ///   本函数只保证执行期的资源就绪；真正的派发录在 `Nanite_CullChain3` 的 pass 体内。
    /// 描述符集只建一次；**每次录制**再把绑定重写一遍（资产/可见簇/实例缓冲与 4 张颜色目标）。
    /// @param targets 本帧的 GBuffer 纹理（用来建第 2 趟的 4 张颜色目标绑定）
    bool EnsureSoftRasterResources(const GBufferTargets& targets);


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
};

} // namespace he::render
