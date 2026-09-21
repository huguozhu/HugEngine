#pragma once

// ============================================================
// Nanite/NaniteStream.{h,cpp} — §14.8 任务 24：LOD 流式（反馈 + 页池）的**宿主**
//
// 【本文件是任务 24 新增的；它做四件事，每一件都对应 §14.32 的一节】
//   ① **页池**（③）：`kNanitePagePoolSlots` 个定长槽，每槽装"一页的簇记录 + 顶点 + 三角形"三段。
//      槽长 = 每页各段条数的**最大值**（`NanitePagePlan::stats` 的 `*Stride`）——
//     池是定长槽的物理数组，任何一个槽都必须装得下最坏的那一页，否则上传就写越界。
//   ② **页表 + 间接层**（③ + 第一处追加）：`NanitePageTableEntry[pageCount]`，携带 `slot` /
//      `vertexBegin` / `triangleBegin` / `resident`。着色器靠它把"共享数组的绝对偏移"换算成
//      "页池内的偏移"。**页表只由 CPU 写**（见下面"与设计的一处偏离"）。
//   ③ **反馈通路**（④）：GPU 发现"页未驻留"就把页号追加进反馈环；CPU 每帧读回**恰好
//      `feedbackLatency` 帧之前**那一个环槽（= 延迟恰好等于常量），合并去重后进上传队列。
//   ④ **驻留管理**（⑤）：池满时按 LRU 淘汰（`lastRequestedFrame` 最旧、且距当前帧 ≥
//      `kNanitePageEvictionSafetyFrames`），每帧上传上限 `pageUploadsPerFrame`。
//
// 【阶段一的数据源（⑦ 的默认项 + 它的硬前置）】页的数据源是**已在内存的完整资产**：
//   `NaniteScene` 在资产上传时把它留存为 CPU 副本（§14.32 的第三处追加点明了这份留存是硬前置），
//   本类只持有它的 `const` 指针并从中拷页 ⇒ **不需要任何磁盘 I/O**，也回避了"Core 层无文件系统
//   封装"这个既有缺口。代价（Sponza **实测 13,405,960 字节 = 12.78 MiB**）在 §14.37 里如实标出。
//
// 【与设计的两处**显式偏离**（都写在 §14.37，不悄悄改口径）】
//   · 设计 ④ 写"去重由页表里的 `requestedFrame == 当前帧` 判定" ⇒ 那要求 **GPU 写页表**，
//     而页表同时被 CPU 每帧写 ⇒ 同一块内存无同步地双边写。本实现改成
//     **GPU 只追加（允许重复）、CPU 读回侧去重**：页表因此**只由 CPU 写**，语义更简单、更快，
//     且"请求了多少条"与"请求了多少个不同页"两个量都能如实报出来。
//   · 设计 ⑤ 写"淘汰前必须确认没有在飞命令引用它（复用 `DeferredDestructionQueue` 或 3 帧延迟）"。
//     本实现两者都做：`kNanitePageEvictionSafetyFrames` 的帧龄门槛 **+** 每帧步进前的一次
//     `WaitIdle()`（它让"在飞"这件事在物理上不存在）。`WaitIdle` 的代价与理由见 §14.37。
//
// 【§14.2 不变式 1】`streaming` 默认关：本类的 `Setup` 由 `NaniteRenderer` **只在
//   `enabled && softRaster && streaming` 三个条件全真时**调用 ⇒ 关闭档一个缓冲都不建、
//   一个字节都不拷、每帧一次调用都不做（`BeginFrame` 在未就绪时直接返回）。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构。
//   本文件只依赖 RHI 与模块自己的 POD。
// ============================================================

#include "Nanite/NaniteTypes.h"
#include "Nanite/NaniteUpload.h"   // `NanitePagePlan` / `NanitePackedAsset`（RHI-free 头）

#include "RHI/RHI.h"

#include <memory>
#include <vector>

namespace he::render {

/// 流式的**退化原因**（读数行 `stream=off reason=<...>` 的取值）
///
/// 【为什么不静默】§14.32 ⑧ 的第二条与任务书的"退化"一栏都要求：池容量 0、资产对不上、
///   内容跨页这类情况下必须**自动关流式并在读数里报原因**。所以每一种退化都有独立的名字，
///   读日志的人不需要猜"为什么没生效"。
enum class NaniteStreamReason : u32 {
    Ok = 0,               ///< 生效（`stream=on`）
    Disabled,             ///< 开关没开（默认档；此时**不打印** stream 行）
    RequiresSoftRaster,   ///< `streaming=1` 但 `softRaster=0`：软光栅没接管 GBuffer，流式无处生效
    PoolZeroSlots,        ///< `pagePoolSlots == 0`：没有槽可放页
    NoAsset,              ///< 资产未入库 / 上传失败（没有数据可拷）
    PlanFailed,           ///< 页划分失败（两张共享内容表不同源等；具体计数见日志）
    PageStraddle,         ///< 出现"一份共享内容跨页"（划分守卫不成立）
    ResourceFailed,       ///< 池/页表/反馈环的缓冲创建失败（显存不足等）
    NoDevice,             ///< 设备为空
};

/// 退化原因的可读名字（读数行直接打印它；`Ok` 返回 `ok`）
[[nodiscard]] const char* NaniteStreamReasonName(NaniteStreamReason reason);

/// 流式的运行参数（全部来自 `NaniteSettings`，由门面在 `Setup` 时传入）
struct NaniteStreamConfig {
    u32 contentsPerPage   = kNanitePageContentsPerPage;      ///< 每页共享内容份数（K）
    u32 poolSlots         = kNanitePagePoolSlotsDefault;     ///< 页池槽数
    u32 feedbackLatency   = kNaniteFeedbackLatencyDefault;   ///< 反馈延迟（帧）
    u32 uploadsPerFrame   = kNanitePageUploadsPerFrameDefault;///< 每帧上传上限（页）
};

/// 给光栅端用的**只读视图**（`NaniteRaster` 把它绑到描述符上）
struct NaniteStreamViews {
    rhi::IRHIBuffer* clusterPage   = nullptr;  ///< `NaniteClusterPageRef[clusterCount]`（8B/条）
    rhi::IRHIBuffer* pageTable     = nullptr;  ///< `NanitePageTableEntry[pageCount]`（16B/条）
    rhi::IRHIBuffer* poolClusters  = nullptr;  ///< 池的簇段（clusterStride 条/槽）
    rhi::IRHIBuffer* poolVertices  = nullptr;  ///< 池的顶点段（vertexStride 条/槽）
    rhi::IRHIBuffer* poolTriangles = nullptr;  ///< 池的三角形段（triangleStride 条/槽）
    rhi::IRHIBuffer* feedback      = nullptr;  ///< 反馈环（RW：GPU 追加请求）
    bool enabled = false;                      ///< false ⇒ 着色器直读资产段（默认档）

    u32 clusterStride  = 0u;   ///< 一个槽的簇记录条数（= 每页最大簇数）
    u32 vertexStride   = 0u;   ///< 一个槽的顶点条数
    u32 triangleStride = 0u;   ///< 一个槽的三角形条数

    [[nodiscard]] bool valid() const {
        return clusterPage && pageTable && poolClusters && poolVertices && poolTriangles && feedback;
    }
};

/// 页池 + 页表 + 反馈 + 驻留管理的宿主（§14.8 任务 24 阶段一）
class NaniteStream {
public:
    NaniteStream() = default;
    ~NaniteStream() = default;

    NaniteStream(const NaniteStream&) = delete;
    NaniteStream& operator=(const NaniteStream&) = delete;

    /// 记住设备并清空状态（重新初始化 = 重新建池）。**不建任何 GPU 资源**
    bool Initialize(rhi::IRHIDevice* device);
    /// 释放全部自持资源（池/页表/反馈环）
    void Shutdown();

    /// 用留存下来的资产建页划分 + 池/页表/反馈环（**只在流式真正生效时调用一次**）
    ///
    /// @param asset 资产的 CPU 副本（调用方保证其生命周期长于本对象，见 `NaniteScene::GetAssetCPUCopy`）
    /// @return true = 流式可用；false = 已按原因退化（`Reason()` 给出原因，**不静默**）
    [[nodiscard]] bool Setup(const NanitePackedAsset& asset, const NaniteStreamConfig& config);

    /// 是否可用（`Setup` 成功且资源齐备）
    [[nodiscard]] bool IsReady() const { return m_Ready; }

    /// **每帧步进**（由门面在帧图构建期调用；未就绪时直接返回，零开销）
    ///
    /// 【同步】内部在**读回反馈之前**做一次 `IRHIDevice::WaitIdle()`。理由：这是"页表由 CPU 写、
    ///   页池由 CPU 拷"这两件事唯一安全的时刻 —— 等掉全部在飞命令之后，才没有命令缓冲会读到
    ///   半写的页表/半拷的页。代价与替代方案见 §14.37 的"存疑未做"。
    /// 【顺序】读回 `latency` 帧之前的请求 → 合并去重 → LRU 淘汰 → 限流上传 → 重写页表 →
    ///   清空本帧要写的那个环槽（CPU 写，等掉在飞命令后同样安全）。
    void BeginFrame(u32 frameIndex);

    /// 光栅端要绑的视图（未就绪时 `enabled == false`）
    [[nodiscard]] const NaniteStreamViews& Views() const { return m_Views; }

    // ── 读数（`stream` 行的每一个字段都在这里；全部是可核对的确定性量）──
    [[nodiscard]] u32 PagesTotal()        const { return m_Plan.pageCount; }
    [[nodiscard]] u32 ResidentPages()     const { return m_ResidentCount; }
    [[nodiscard]] u32 PoolSlots()         const { return m_Config.poolSlots; }
    [[nodiscard]] u32 UploadsThisFrame()  const { return m_UploadsThisFrame; }
    [[nodiscard]] u32 EvictedTotal()      const { return m_EvictedTotal; }
    [[nodiscard]] u32 PagesRequestedThisFrame() const { return m_PagesRequestedThisFrame; }
    [[nodiscard]] u32 RequestOverflowTotal()    const { return m_RequestOverflowTotal; }
    /// 累计"从延迟反馈里读出的请求条数"（含重复页）——**非空洞守卫**：池足够大时它必然 > 0，
    /// 因为开局的每一页都必须先被请求一次才会驻留（判据 (d1) 要求 `page_requests > 0`）。
    [[nodiscard]] u32 RequestsTotal()           const { return m_RequestsTotal; }
    [[nodiscard]] u32 MaxRequestsPerFrame()     const { return m_MaxRequestsPerFrame; }
    /// **累计入队页次数**（同一页每次"从非驻留变回待上传"都会再计一次；池足够大时它等于
    /// 不同页数，池小于工作集时会远大于页数）——非空洞守卫：它必然 > 0。
    [[nodiscard]] u32 PagesRequestedTotal()     const { return m_PagesRequestedTotal; }
    [[nodiscard]] u32 FeedbackLatency()   const { return m_Config.feedbackLatency; }
    [[nodiscard]] u32 UploadsPerFrameLimit() const { return m_Config.uploadsPerFrame; }
    [[nodiscard]] u32 ContentsPerPage()   const { return m_Config.contentsPerPage; }
    [[nodiscard]] u32 SlotClusterBytes()  const { return (u32)m_Plan.stats.slotClusterBytes; }
    [[nodiscard]] u32 SlotVertexBytes()   const { return (u32)m_Plan.stats.slotVertexBytes; }
    [[nodiscard]] u32 SlotTriangleBytes() const { return (u32)m_Plan.stats.slotTriangleBytes; }
    [[nodiscard]] usize PoolBytes()       const { return m_Plan.PoolBytes(m_Config.poolSlots); }

    /// 两条自洽判据（验收 (b)）：① 驻留页 + 未驻留页 == 页总数；② 没有两个页共用一个槽。
    /// 【为什么做成函数而不是两个 bool 成员】它们是**从当前状态算出来**的，缓存成成员就有
    ///   "改了状态忘了更新标志"的空间；这里每次读都重算，成本是 O(页数)（Sponza 约 17 页）。
    [[nodiscard]] bool PageTableSelfConsistent(u32& outResident, u32& outNonResident,
                                               u32& outDuplicateSlots) const;

    /// **Map 页表缓冲**数出"真的被标成驻留"的条数（真实 GPU 侧内容，不是 CPU 的账）
    ///
    /// 【为什么需要它】验收 (b) 要求"页表自洽"，而 CPU 侧的 `m_SlotOfPage` 只是**本地的账**：
    ///   若页表缓冲的写入路径断了（Map 失败、偏移算错、被后续覆盖），CPU 的账仍然"自洽"，
    ///   着色器却一页都看不到 —— 那就成了空洞通过。把这个数读回来与 CPU 的账并列打印，
    ///   两者不等就是硬失败。
    /// 【同步约定】只 Map、不等待；调用方必须已 `WaitIdle()`（dump 路径已有）。
    [[nodiscard]] u32 ReadbackGpuResidentPages();

    [[nodiscard]] NaniteStreamReason Reason() const { return m_Reason; }
    [[nodiscard]] const char*        ReasonName() const { return NaniteStreamReasonName(m_Reason); }
    [[nodiscard]] const NanitePagePlan& Plan() const { return m_Plan; }

private:
    /// 建池/页表/反馈环/簇→页映射（`Setup` 的第二步）
    bool CreateResources();
    /// 把一页的三段拷进槽 `slot`（CPU memcpy；数据源是留存的资产）
    void UploadPage(u32 page, u32 slot);
    /// 找一个可用槽：优先空闲槽，其次按 LRU 淘汰（无候选时返回 `kInvalidNanitePageSlot`）
    u32  AcquireSlot(u32 frameIndex);

    rhi::IRHIDevice* m_Device = nullptr;

    NaniteStreamConfig m_Config;
    NanitePagePlan     m_Plan;      ///< CPU 侧页划分（纯函数的产物）
    NaniteStreamViews  m_Views;     ///< 给光栅端的视图
    bool               m_Ready  = false;
    NaniteStreamReason m_Reason = NaniteStreamReason::Disabled;

    /// 页数据的来源（**非持有**：指向 `NaniteScene` 留存的资产 CPU 副本）
    const NanitePackedAsset* m_Asset = nullptr;

    // ── GPU 资源 ──
    std::unique_ptr<rhi::IRHIBuffer> m_PoolClusters;   ///< clusterStride × slots 条簇记录
    std::unique_ptr<rhi::IRHIBuffer> m_PoolVertices;   ///< vertexStride  × slots 条量化顶点
    std::unique_ptr<rhi::IRHIBuffer> m_PoolTriangles;  ///< triangleStride× slots 条打包三角形
    std::unique_ptr<rhi::IRHIBuffer> m_ClusterPage;    ///< 簇 → 页（只读；上传后不再变）
    std::unique_ptr<rhi::IRHIBuffer> m_PageTable;      ///< 页表（**只由 CPU 写**，每帧重写）
    /// 反馈环：`feedbackLatency` 个缓冲。帧 N 用 `N % L` 槽，同时读的也是它
    /// （它上一次被写是帧 N-L —— 这正是"延迟恰好等于常量"的实现方式）。
    std::vector<std::unique_ptr<rhi::IRHIBuffer>> m_FeedbackRing;

    // ── CPU 侧驻留状态 ──
    std::vector<u32> m_SlotOfPage;          ///< 页 → 槽（`kInvalidNanitePageSlot` = 未驻留）
    std::vector<u32> m_PageOfSlot;          ///< 槽 → 页（`kInvalidNanitePageSlot` = 空闲）
    std::vector<u32> m_LastRequestedFrame;  ///< 页 → 最近一次被请求的帧（LRU 的键）
    std::vector<u8>  m_Queued;              ///< 页 → 是否已在上传队列里（去重）
    std::vector<u32> m_UploadQueue;         ///< 待上传页（FIFO；按请求到达顺序 = 优先级）
    u32              m_UploadQueueHead = 0u;///< 队列头（用下标弹出，避免每次都 erase 整段）
    /// "收集后的簇记录序 → 原簇下标"的反查表（装一页的簇段时用；一次性构建）
    std::vector<u32> m_CollectedClusterIndex;
    std::vector<NanitePageTableEntry> m_TableCPU;  ///< 页表的主机镜像（每帧按需上传）

    u32 m_FrameIndex           = 0u;   ///< 当前帧号（上传页时写进 `lastRequestedFrame`）
    u32 m_ResidentCount        = 0u;
    u32 m_EvictedTotal         = 0u;
    u32 m_UploadsThisFrame     = 0u;
    u32 m_PagesRequestedThisFrame = 0u;
    u32 m_RequestOverflowTotal = 0u;
    u32 m_RequestsTotal        = 0u;   ///< 累计读出的请求条数（含重复页；非空洞守卫）
    u32 m_MaxRequestsPerFrame  = 0u;   ///< 单帧请求条数的最大值（同上）
    u32 m_PagesRequestedTotal  = 0u;   ///< **累计入队页次数**（同页可重复计；池小于工作集时会远大于页数）
    u32 m_TableDirty           = 0u;   ///< 页表是否有未上传的改动
};

} // namespace he::render
