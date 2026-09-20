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
    /// 开启时注册三个 pass：`Nanite_InstanceCull` + `Nanite_Cull` + `Nanite_Raster`
    /// （原序 12 个 pass 一个不动）；【任务 14】再追加 `Nanite_ClusterBVH`（per-instance cluster
    /// BVH 的深度优先遍历，只读写模块自持缓冲 ⇒ 不声明任何帧图资源）；
    /// 【§14.8 任务 6】`meshTest` 为真时**再追加**一个 `Nanite_MeshTest`（mesh PSO 通道）。
    ///
    /// 【§14.8 任务 13 的 camera 参数】实例剔除需要世界空间视锥与相机位置：
    ///   `NaniteCull::SetInstanceCullFrame` 由 view-proj 提取 6 平面、并以相机位置为基准生成
    ///   合成实例网格（来源/坐标系见 `NaniteCull.h` 的 `SetInstanceCullFrame` 注释）。
    ///   【为什么从调用方传入】相机的唯一持有者是 `DeferredPipeline`；模块不自造也没有别处可取。
    void AddPasses(RenderGraph& rg, const NaniteGBufferHandles& gb, const CameraData& camera);

    /// 【§14.8 任务 4：GBuffer 之后的后置挂钩】在 **GBuffer 几何段结束之后、任何读取 GBuffer
    /// 的 pass（SSAO/Lighting 等）之前**调用第二处注册点：
    ///   · 为什么必须有这一处：`AddPasses` 的位置在既有 `GB_Clear` **之前**，模块的写入会被
    ///     `GB_Clear` 覆盖，无法被同帧的 Lighting 读到；任务 4 的验收（"compute 写 GBuffer 且
    ///     同帧被 Lighting 读到"）只能在 GBuffer 之后注册才能成立。
    ///   · 开关守卫与 `AddPasses` 是**同一个真值**（`NaniteSettings::enabled` + `IsReady()`），
    ///     不是新门控；`nanite_test_write` 只是模块内部的第二个条件。
    ///   · 关闭档 / 未就绪 / `testWrite=false` ⇒ 一个 pass 都不注册（§14.2 不变式 1）。
    ///   · 注意：这里写的是 `gbAlbedo` 的 UAV，因此**不能**声明 `gbDepth/gbWorldPos` 的那组
    ///     WAW —— `GB_Clear` 已经在前面写过它们，再声明只会多出一条无意义的依赖。
    ///
    /// 【将来】任务 26 的调试可视化落点也在这里（GBuffer 之后的可视化叠加）。
    void AddPostGBufferPasses(RenderGraph& rg, const NaniteGBufferHandles& gb);

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
    /// 命令条数（读回时按"非哨兵且字段合法"统计）；Z = 绘制端片元原子计数的值。
    /// 任务 3 的验收就是 X == Y == Z == N。
    ///
    /// 【同步约定】本函数**只做 Map 读回，不做任何等待** —— 调用方必须在 GPU 完成后调用
    /// （样例的 dump 路径已经有 `device->WaitIdle()`，照抄既有白炉探针/落盘的读数方式）。
    /// 关闭档下直接返回（不打印），保证关闭档日志与基线一致。
    void LogFakePipelineReadback();

    /// 【§14.8 任务 13】dump 帧打印**恰好一行**实例剔除的 GPU/CPU 逐项对照：    ///   `[Nanite] instance_cull gpu=<k> cpu=<m> mismatch=0 first=<i0,i1,...>`
    ///
    /// · `gpu` = GPU 读回的可见实例计数（`Nanite_InstanceCull` 的计数缓冲）；
    /// · `cpu` = CPU 参考剔除（`NaniteCullInstancesCPU`）的可见数；
    /// · `mismatch` = 两个可见**集合**的逐项差异数（含条数差）；
    /// · `first` = 排序后的 GPU 可见列表前若干个下标（可核对的样本；空列表打 `-`）。
    ///
    /// 【为什么比较前要排序】GPU 用"原子取槽位"做压缩，槽位分配顺序与线程调度相关，
    ///   同一个可见集合可能有不同的列表顺序；CPU 参考是升序紧凑的。故比较口径是
    ///   **排序后的逐项相等**（集合等价），顺序本身不是语义（任务 14+ 也不依赖顺序）。
    ///
    /// 【同步约定】与 `LogFakePipelineReadback` 相同：只做 Map 读回、不做等待；调用方必须已
    /// `WaitIdle()`。关闭档 / 未就绪时直接返回、不打印 —— 保证关闭档日志与基线一致。
    void LogInstanceCullReadback();

    /// 【§14.8 任务 14】dump 帧打印**恰好一行** per-instance cluster BVH 的读数：
    ///   `[Nanite] cluster_bvh nodes=<N> depth=<D> gpu_visited=<V> cpu_visited=<V>
    ///    gpu_clusters=<C> cpu_clusters=<C> mismatch=<M>`
    ///
    /// · `nodes` / `depth` = CPU 构建出的 BVH 节点数与最大深度（**同一份数据**也上传给了 GPU）；
    /// · `gpu_visited` = GPU 读回的"已访问节点数"（原子累加；**真实 GPU 读回**）；
    /// · `cpu_visited` = CPU 参考遍历（`NaniteTraverseClusterBVHCPU`）的同一读数；
    /// · `gpu_clusters` = GPU 读回的"可见簇引用数"（原子累加）；`cpu_clusters` = CPU 参考同一读数；
    /// · `mismatch` = 两个可见簇**集合**的逐项差异数（含条数差）—— 不是只比计数。
    ///
    /// 【比较口径】GPU 用"原子取槽位"压缩 ⇒ 列表顺序不定；两边都按 (instance, cluster) 排序后
    ///   逐项比较（集合等价），与任务 13 相同。
    /// 【容量截断】可见簇引用表容量 = `kNaniteMaxBVHInstances × kNaniteMaxBVHClusters`
    ///   （正常配置下 53 万 < 105 万 ⇒ **不截断**）；两个计数都先按容量截断再比较，口径一致。
    /// 【CPU 参考为什么在这里算（而不是每帧在 `RecordClusterBVHPass` 里算）】BVH 遍历的成本是
    ///   实例域 × 全簇数（默认 64 × 8287 ≈ 53 万次球测试），每帧跑一遍会拖慢开启档；而 dump 帧的
    ///   读回紧跟在 `WaitIdle()` 之后、期间没有录制新帧 ⇒ 这里的 CPU 输入正是被读回那一帧的输入
    ///   （同一个视锥、同一张实例表、同一个实例域），仍然是"同帧同输入"的比较。
    ///
    /// 【同步约定】与 `LogFakePipelineReadback` 相同：只做 Map 读回、不做等待；调用方必须已
    /// `WaitIdle()`。关闭档 / 未就绪时直接返回、不打印 —— 保证关闭档日志与基线一致。
    void LogClusterBVHReadback();

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

    /// 开关与档位的唯一真值（默认 `enabled = false` ⇒ §14.2 不变式 1）
    NaniteSettings m_Settings;

    // 模块内部各段（§14.3 的六个文件各司其职，本类只做门面与生命周期转发）
    NaniteScene  m_Scene;
    NaniteUpload m_Upload;
    NaniteCull   m_Cull;
    NaniteRaster m_Raster;
};

} // namespace he::render
