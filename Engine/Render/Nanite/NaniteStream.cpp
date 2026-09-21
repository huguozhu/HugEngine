// ============================================================
// Nanite/NaniteStream.cpp — §14.8 任务 24：页池 / 页表 / 反馈 / 驻留管理的实现
//
// 【口径与偏离的完整说明在 `NaniteStream.h` 的文件头】这里只留与代码逐句对应的短注释。
// 【一句话的数据流】
//   GPU（软光栅第 1 趟）发现"页未驻留" → 原子追加页号到反馈环 → CPU 每帧在帧图构建期
//   `WaitIdle` 后读回 `latency` 帧之前的那一个环槽 → 合并去重 → LRU 淘汰 → 限流上传 →
//   重写页表 → 清空本帧要用的环槽。着色器只读页表与页池，**永远不写页表**。
// ============================================================

#include "Nanite/NaniteStream.h"

#include "Core/Log.h"

#include <algorithm>   // std::min / std::memcpy 之前的 std::fill
#include <cstring>     // std::memcpy / std::memset（页池与页表的上传）

namespace he::render {

const char* NaniteStreamReasonName(NaniteStreamReason reason) {
    switch (reason) {
        case NaniteStreamReason::Ok:                 return "ok";
        case NaniteStreamReason::Disabled:           return "disabled";
        case NaniteStreamReason::RequiresSoftRaster: return "requires_soft_raster";
        case NaniteStreamReason::PoolZeroSlots:      return "pool_zero_slots";
        case NaniteStreamReason::NoAsset:            return "no_asset";
        case NaniteStreamReason::PlanFailed:         return "plan_failed";
        case NaniteStreamReason::PageStraddle:       return "page_straddle";
        case NaniteStreamReason::ResourceFailed:     return "resource_failed";
        case NaniteStreamReason::NoDevice:           return "no_device";
    }
    return "unknown";
}

bool NaniteStream::Initialize(rhi::IRHIDevice* device) {
    Shutdown();
    m_Device = device;
    // 【默认档就是"没开"】`streaming=false` 时本类只被 Initialize 一次，此后每帧的
    //   `BeginFrame` 在 `!m_Ready` 上直接返回 ⇒ 零每帧开销（§14.2 不变式 1）。
    m_Reason = (device != nullptr) ? NaniteStreamReason::Disabled : NaniteStreamReason::NoDevice;
    return m_Device != nullptr;
}

void NaniteStream::Shutdown() {
    m_FeedbackRing.clear();
    m_PoolClusters.reset();
    m_PoolVertices.reset();
    m_PoolTriangles.reset();
    m_ClusterPage.reset();
    m_PageTable.reset();

    m_Config = NaniteStreamConfig{};
    m_Plan   = NanitePagePlan{};
    m_Views  = NaniteStreamViews{};
    m_Asset  = nullptr;
    m_Ready  = false;
    m_Reason = NaniteStreamReason::Disabled;

    m_SlotOfPage.clear();
    m_PageOfSlot.clear();
    m_LastRequestedFrame.clear();
    m_Queued.clear();
    m_UploadQueue.clear();
    m_UploadQueueHead = 0u;
    m_CollectedClusterIndex.clear();
    m_TableCPU.clear();

    m_ResidentCount           = 0u;
    m_EvictedTotal            = 0u;
    m_UploadsThisFrame        = 0u;
    m_PagesRequestedThisFrame = 0u;
    m_RequestOverflowTotal    = 0u;
    m_RequestsTotal           = 0u;
    m_MaxRequestsPerFrame     = 0u;
    m_PagesRequestedTotal     = 0u;
    m_TableDirty              = 0u;
    m_FrameIndex              = 0u;

    m_Device = nullptr;
}

bool NaniteStream::Setup(const NanitePackedAsset& asset, const NaniteStreamConfig& config) {
    // 【重新 Setup 是允许的】（资产重传/改档），先释放旧资源但保留设备指针
    rhi::IRHIDevice* device = m_Device;
    const bool deviceOk = (device != nullptr);
    Shutdown();
    m_Device = device;
    if (!deviceOk) {
        m_Reason = NaniteStreamReason::NoDevice;
        return false;
    }

    // ── ① 退化守卫（每一种都**不静默**：`Reason()` 会被读数行打印出来）──
    if (config.poolSlots == 0u) {
        m_Reason = NaniteStreamReason::PoolZeroSlots;
        HE_CORE_WARN("[Nanite] stream=off reason={}（页池槽数 = 0 ⇒ 没有页能驻留；"
                     "这不是错误，但流式在本档没有任何意义）", NaniteStreamReasonName(m_Reason));
        return false;
    }
    if (asset.clusters.empty() || asset.vertices.empty() || asset.triangles.empty()) {
        m_Reason = NaniteStreamReason::NoAsset;
        HE_CORE_WARN("[Nanite] stream=off reason={}（簇 {} / 顶点 {} / 三角形 {}）",
                     NaniteStreamReasonName(m_Reason), (u32)asset.clusters.size(),
                     (u32)asset.vertices.size(), (u32)asset.triangles.size());
        return false;
    }

    // ── ② 页划分（纯函数；页边界完全由资产自身推出，见 `NaniteUpload.h`）──
    m_Config = config;
    if (!BuildNanitePagePlan(asset.clusters, (u32)asset.vertices.size(),
                             (u32)asset.triangles.size(), config.contentsPerPage, m_Plan)) {
        m_Reason = NaniteStreamReason::PlanFailed;
        HE_CORE_WARN("[Nanite] stream=off reason={}（页划分失败：簇 {}）",
                     NaniteStreamReasonName(m_Reason), (u32)asset.clusters.size());
        return false;
    }
    // 【守卫：每份共享内容不跨页】设计 ② 要求上传期校验这一条；本实现按构造保证成立，
    //   仍然真的查一遍 —— 它把"分页口径被改坏"变成可读的退化，而不是静默错画。
    if (m_Plan.stats.pageStraddleCount != 0u) {
        m_Reason = NaniteStreamReason::PageStraddle;
        HE_CORE_WARN("[Nanite] stream=off reason={}（有 {} 个簇引用的共享内容跨了页；"
                     "页划分必须对齐到整份内容边界）",
                     NaniteStreamReasonName(m_Reason), m_Plan.stats.pageStraddleCount);
        return false;
    }

    m_Asset = &asset;   // 非持有：指向 `NaniteScene` 留存的资产 CPU 副本

    // ── ③ 建池 / 页表 / 簇→页 / 反馈环 ──
    if (!CreateResources()) {
        m_Reason = NaniteStreamReason::ResourceFailed;
        HE_CORE_ERROR("[Nanite] stream=off reason={}（池 {} 槽 × {} 字节/槽 的三段缓冲建不起来）",
                      NaniteStreamReasonName(m_Reason), config.poolSlots,
                      (unsigned long long)m_Plan.PoolBytes(config.poolSlots));
        m_Asset = nullptr;
        m_Plan  = NanitePagePlan{};
        return false;
    }

    m_Ready  = true;
    m_Reason = NaniteStreamReason::Ok;
    // 【恰好一行】流式建成的自证：页划分的**全部**实测读数一次打全（`stream` 行只报运行期状态）
    // 【`retained_*` 而不是 `asset_bytes`】资产 CPU 副本的 `bytes` 字节镜像在留存时已被丢弃
    //   （它的唯一消费者是上传时的逐字节读回校验），所以"这份资产占多少内存"要用**剩下的段**算。
    const usize retainedBytes = (usize)asset.clusters.size()  * sizeof(NaniteClusterRecord)
                              + (usize)asset.vertices.size()  * sizeof(NaniteVertex)
                              + (usize)asset.triangles.size() * sizeof(NanitePackedTriangle)
                              + (usize)asset.materials.size() * sizeof(NaniteMaterialRecord);
    HE_CORE_INFO("[Nanite] stream_setup contents={} pages={} clusters={} K={} pool_slots={} "
                 "strides=(cluster={} vertex={} tri={}) slot_bytes=(c={} v={} t={}) pool_bytes={} "
                 "retained_bytes={} latency={} uploads_per_frame={}",
                 m_Plan.contentCount, m_Plan.pageCount, m_Plan.clusterCount,
                 m_Config.contentsPerPage, m_Config.poolSlots,
                 m_Plan.stats.clusterStride, m_Plan.stats.vertexStride, m_Plan.stats.triangleStride,
                 (unsigned long long)m_Plan.stats.slotClusterBytes,
                 (unsigned long long)m_Plan.stats.slotVertexBytes,
                 (unsigned long long)m_Plan.stats.slotTriangleBytes,
                 (unsigned long long)PoolBytes(),
                 (unsigned long long)retainedBytes,
                 m_Config.feedbackLatency, m_Config.uploadsPerFrame);
    return true;
}

bool NaniteStream::CreateResources() {
    if (!m_Device || !m_Asset) return false;

    const u32 slots = m_Config.poolSlots;
    const NanitePagePlanStats& ps = m_Plan.stats;

    // ── 1. 簇 → 页映射（只读；上传后不再变）：每簇 8B ──
    {
        rhi::BufferDesc d;
        d.size = (usize)m_Plan.clusterPage.size() * sizeof(NaniteClusterPageRef);
        d.usage = rhi::BufferUsage::Storage;
        d.cpuAccess = false;
        d.initialData = m_Plan.clusterPage.data();
        m_ClusterPage = m_Device->CreateBuffer(d);
        if (!m_ClusterPage) return false;
    }

    // ── 2. 页表（**只由 CPU 写**；每帧按需重写整表）：每页 16B ──
    // 初始状态 = 全部未驻留。第 0 帧因此"一个页都不在池里"，所有可见簇都计缺页
    //（这正是验收 (c) 想要的起点，也让"缺页 ⇒ 少画"这条行为在日志里立刻可见）。
    {
        m_TableCPU.assign(m_Plan.pageCount, NanitePageTableEntry{ kInvalidNanitePageSlot, 0u, 0u, 0u });
        rhi::BufferDesc d;
        d.size = (usize)m_Plan.pageCount * sizeof(NanitePageTableEntry);
        d.usage = rhi::BufferUsage::Storage;
        d.cpuAccess = true;                 // 每帧重写（等掉在飞命令之后才写，见 BeginFrame）
        d.initialData = m_TableCPU.data();
        m_PageTable = m_Device->CreateBuffer(d);
        if (!m_PageTable) return false;
    }

    // ── 3. 页池三段（每段都是"定长槽"的扁平数组）──
    // 【槽长取"每页最大条数"】池是物理定长槽：任何一个槽都必须装得下最坏的那一页。
    //   实测的池足迹在 `stream_setup` 行里（报告里与资产字节数对比）。
    {
        rhi::BufferDesc d;
        d.size      = (usize)slots * ps.clusterStride * sizeof(NaniteClusterRecord);
        d.usage     = rhi::BufferUsage::Storage;
        d.cpuAccess = true;                  // CPU 拷页（`UploadPage`）
        m_PoolClusters = m_Device->CreateBuffer(d);
        if (!m_PoolClusters) return false;

        d.size = (usize)slots * ps.vertexStride * sizeof(NaniteVertex);
        m_PoolVertices = m_Device->CreateBuffer(d);
        if (!m_PoolVertices) return false;

        d.size = (usize)slots * ps.triangleStride * sizeof(NanitePackedTriangle);
        m_PoolTriangles = m_Device->CreateBuffer(d);
        if (!m_PoolTriangles) return false;
    }

    // ── 4. 反馈环：`latency` 个缓冲（帧 N 用槽 N % L；CPU 在帧 N 读的正是它 —— 它上一次被写
    //      是帧 N-L ⇒ **延迟恰好等于常量**，不需要任何额外的时间戳机制）──
    {
        m_FeedbackRing.clear();
        m_FeedbackRing.reserve(m_Config.feedbackLatency);
        // 【缓冲大小 = 头 2 + 环 1024 + **戳记 pageCount**】戳记数组（GPU 独占写）用来把
        //   "同一页每帧只请求一次"做在 GPU 侧，避免环被同一页的重复请求挤满（见 shader 的说明）。
        const usize words = (usize)kNanitePageFeedbackWords + m_Plan.pageCount;
        std::vector<u32> zero(words, 0u);
        for (u32 i = 0u; i < m_Config.feedbackLatency; ++i) {
            rhi::BufferDesc d;
            d.size        = words * sizeof(u32);
            d.usage       = rhi::BufferUsage::Storage;   // shader 侧是 RWStructuredBuffer
            d.cpuAccess   = true;                        // CPU 读回请求 + 每帧清空
            d.initialData = zero.data();
            auto buf = m_Device->CreateBuffer(d);
            if (!buf) return false;
            m_FeedbackRing.push_back(std::move(buf));
        }
    }

    // ── 5. CPU 侧驻留状态 ──
    m_SlotOfPage.assign(m_Plan.pageCount, kInvalidNanitePageSlot);
    m_PageOfSlot.assign(slots, kInvalidNanitePageSlot);
    m_LastRequestedFrame.assign(m_Plan.pageCount, 0u);
    m_Queued.assign(m_Plan.pageCount, 0u);
    m_UploadQueue.clear();
    m_UploadQueueHead = 0u;

    // ── 6. "收集后的簇记录序 → 原簇下标"的反查表（上传一页的簇段时用）──
    // 【为什么要它】簇段是**收集**的（同页的簇出现记录在簇下标空间里不连续），所以装页必须
    //   按"收集序"去取原记录：`收集位置 = pageClusterBegin[page] + clusterPage[i].local`。
    //   这张反查表就是那个映射的展开（一次性 O(clusterCount)）。
    m_CollectedClusterIndex.assign(m_Plan.clusterCount, 0u);
    for (u32 i = 0u; i < m_Plan.clusterCount; ++i) {
        const u32 page = m_Plan.clusterPage[i].page;
        const u32 pos  = m_Plan.pageClusterBegin[page] + m_Plan.clusterPage[i].local;
        m_CollectedClusterIndex[pos] = i;
    }

    // ── 7. 给光栅端的视图 ──
    m_Views.clusterPage   = m_ClusterPage.get();
    m_Views.pageTable     = m_PageTable.get();
    m_Views.poolClusters  = m_PoolClusters.get();
    m_Views.poolVertices  = m_PoolVertices.get();
    m_Views.poolTriangles = m_PoolTriangles.get();
    m_Views.feedback      = m_FeedbackRing.empty() ? nullptr : m_FeedbackRing[0].get();
    m_Views.clusterStride  = ps.clusterStride;
    m_Views.vertexStride   = ps.vertexStride;
    m_Views.triangleStride = ps.triangleStride;
    m_Views.enabled        = true;
    return m_Views.valid();
}

void NaniteStream::UploadPage(u32 page, u32 slot) {
    if (!m_Asset || page >= m_Plan.pageCount || slot >= m_Config.poolSlots) return;
    const NanitePagePlanStats& ps = m_Plan.stats;

    // ── ① 簇记录：按**收集序**从资产簇段取，写到槽的簇段起点 ──
    // 【Map 失败必须**不**把这一页标成驻留】否则着色器会去读一块从未写入（= 未初始化）的池内存：
    //   里面的 `triangleCount` 是任意值，而它直接决定软光栅那个 `for` 的迭代次数 ——
    //   最坏情况下是 4G 次 × 每次跑一遍屏幕包围盒 ⇒ **GPU 看门狗超时、整帧挂死**，
    //   而 CPU 侧只表现为 `WaitIdle` 永久阻塞、没有任何错误输出（极难定位）。
    //   ⇒ 这里改成"先把三段都写成功，再改驻留状态"。
    bool ok = false;
    {
        const u32 begin = m_Plan.pageClusterBegin[page];
        const u32 count = m_Plan.pageClusterCount[page];
        if (void* p = m_PoolClusters->Map()) {
            NaniteClusterRecord* dst = static_cast<NaniteClusterRecord*>(p);
            for (u32 k = 0u; k < count; ++k) {
                dst[(usize)slot * ps.clusterStride + k] =
                    m_Asset->clusters[m_CollectedClusterIndex[begin + k]];
            }
            m_PoolClusters->Unmap();
            ok = true;
        }
    }
    // ── ② 顶点段 / 三角形段：资产里的**连续区间**原样拷（这正是"页 = 区间"的直接好处）──
    if (ok) {
        ok = false;
        const u32 vBegin = m_Plan.pageVertexBegin[page];
        const u32 vCount = m_Plan.pageVertexCount[page];
        const u32 tBegin = m_Plan.pageTriangleBegin[page];
        const u32 tCount = m_Plan.pageTriangleCount[page];
        if (void* p = m_PoolVertices->Map()) {
            NaniteVertex* dst = static_cast<NaniteVertex*>(p);
            std::memcpy(dst + (usize)slot * ps.vertexStride,
                        m_Asset->vertices.data() + vBegin,
                        (usize)vCount * sizeof(NaniteVertex));
            m_PoolVertices->Unmap();
            ok = true;
        }
        if (ok) {
            ok = false;
            if (void* p = m_PoolTriangles->Map()) {
                NanitePackedTriangle* dst = static_cast<NanitePackedTriangle*>(p);
                std::memcpy(dst + (usize)slot * ps.triangleStride,
                            m_Asset->triangles.data() + tBegin,
                            (usize)tCount * sizeof(NanitePackedTriangle));
                m_PoolTriangles->Unmap();
                ok = true;
            }
        }
    }
    if (!ok) {
        // 【不静默】三段里任一段没写成就放弃这一页：它保持"非驻留"，下一帧会被重新请求
        static bool warned = false;
        if (!warned) {
            warned = true;
            HE_CORE_ERROR("[Nanite] 页 {} 的槽 {} 写入失败（池缓冲不可映射）——本页保持非驻留并"
                          "在下一次请求时重试；**不会**把未初始化的池内存当成几何", page, slot);
        }
        // 清掉可能已部分写入的半份数据对应的页表项（保持"非驻留"）
        m_TableCPU[page] = NanitePageTableEntry{ kInvalidNanitePageSlot, 0u, 0u, 0u };
        m_TableDirty = 1u;
        return;
    }

    // ── ③ CPU 侧状态与页表（走到这里才代表"槽里的三段都已经是这一页的数据"）──
    m_SlotOfPage[page]        = slot;
    m_PageOfSlot[slot]        = page;
    m_LastRequestedFrame[page] = m_FrameIndex;   // 刚装进来的页不该被立刻淘汰
    m_TableCPU[page].slot          = slot;
    m_TableCPU[page].vertexBegin   = m_Plan.pageVertexBegin[page];
    m_TableCPU[page].triangleBegin = m_Plan.pageTriangleBegin[page];
    m_TableCPU[page].resident      = 1u;
    m_TableDirty = 1u;
    ++m_ResidentCount;
}

u32 NaniteStream::AcquireSlot(u32 frameIndex) {
    // ── ① 空闲槽优先（线性扫描；槽数 ≤ `kNanitePagePoolSlotsMax`）──
    for (u32 s = 0u; s < m_Config.poolSlots; ++s) {
        if (m_PageOfSlot[s] == kInvalidNanitePageSlot) return s;
    }
    // ── ② 池满 ⇒ LRU 淘汰：`lastRequestedFrame` 最旧、且帧龄 ≥ 安全延迟 ──
    // 【为什么还要帧龄门槛（等掉在飞命令之后其实已经安全）】它同时挡住"刚被请求就被踢掉"
    //   的抖动：请求是有延迟的（`feedbackLatency` 帧），帧龄门槛保证一个页至少活过
    //   请求 → 读回 → 上传 这条链，不会在同一帧内被自己挤出去。
    u32 victim = kInvalidNanitePageSlot;
    u32 victimFrame = 0xFFFFFFFFu;
    for (u32 p = 0u; p < m_Plan.pageCount; ++p) {
        if (m_SlotOfPage[p] == kInvalidNanitePageSlot) continue;
        const u32 last = m_LastRequestedFrame[p];
        if (last + kNanitePageEvictionSafetyFrames > frameIndex) continue;   // 太新：本帧不许动
        if (last < victimFrame) { victimFrame = last; victim = p; }
    }
    if (victim == kInvalidNanitePageSlot) return kInvalidNanitePageSlot;

    const u32 slot = m_SlotOfPage[victim];
    m_SlotOfPage[victim] = kInvalidNanitePageSlot;
    m_PageOfSlot[slot]   = kInvalidNanitePageSlot;
    m_TableCPU[victim]   = NanitePageTableEntry{ kInvalidNanitePageSlot, 0u, 0u, 0u };
    m_TableDirty = 1u;
    --m_ResidentCount;
    ++m_EvictedTotal;
    return slot;
}

void NaniteStream::BeginFrame(u32 frameIndex) {
    m_FrameIndex = frameIndex;
    m_UploadsThisFrame        = 0u;
    m_PagesRequestedThisFrame = 0u;
    if (!m_Ready) return;   // 默认档（未开流式）零开销：一个字节都不读、一次同步都不做

    // ── ⓪ 同步点：等掉全部在飞命令 ──
    // 【为什么必须有这一步（而不是"只在 dump 帧等"）】页表与页池都是**CPU 写、GPU 读**。
    //   只有"没有任何在飞命令"时重写它们才是无竞态的；本引擎的缓冲是持久映射的 host-visible
    //   内存（`VulkanResources.cpp` 的 VMA 参数），CPU 写与 GPU 读之间没有隐式同步。
    //   【代价与替代方案】见 §14.37 的"存疑未做"：它把 CPU/GPU 的并行度压成串行，
    //   本样例整帧本来就 CPU 受限（§14.36 实测墙钟 126~146 ms vs GPU 9~43 ms），
    //   所以对"阶段一的功能验收"没有影响；真正的流水线化留给阶段二。
    m_Device->WaitIdle();

    // ── ① 读回 `latency` 帧之前的请求（环槽 = frame % L；它上次被写正是帧 frame-L）──
    if (!m_FeedbackRing.empty()) {
        const u32 ring = frameIndex % m_Config.feedbackLatency;
        // 【视图必须跟着本帧的环槽走】帧 N 的 GPU 请求要写进环槽 N % L，而 CPU 在帧 N+L 读它
        //   ⇒ "请求到驻留的延迟"就**恰好**是 `feedbackLatency`，不需要任何时间戳。
        m_Views.feedback = m_FeedbackRing[ring].get();
        rhi::IRHIBuffer* buf = m_FeedbackRing[ring].get();
        if (void* p = buf->Map()) {
            const u32* w = static_cast<const u32*>(p);
            u32 count = w[kNanitePageFeedbackCountSlot];
            m_RequestOverflowTotal += w[kNanitePageFeedbackOverflowSlot];
            if (count > kNanitePageRequestSlots) count = kNanitePageRequestSlots;   // 防御：坏数据
            // 累计口径（非空洞守卫）：池足够大时 `requests_total` 必然 > 0 —— 开局的每一页
            // 都必须先被请求一次才会驻留；`pages_requested_total` 最终应等于 `pages_total`。
            m_RequestsTotal += count;
            if (count > m_MaxRequestsPerFrame) m_MaxRequestsPerFrame = count;
            for (u32 i = 0u; i < count; ++i) {
                const u32 page = w[kNanitePageFeedbackRingOffset + i];
                if (page >= m_Plan.pageCount) continue;                    // 非法页号：丢弃
                if (m_SlotOfPage[page] != kInvalidNanitePageSlot) continue;// 已驻留：无需再传
                m_LastRequestedFrame[page] = frameIndex;                   // 刷新 LRU 键
                ++m_PagesRequestedThisFrame;
                if (m_Queued[page] == 0u) {                                 // **CPU 侧去重**
                    m_Queued[page] = 1u;
                    ++m_PagesRequestedTotal;
                    m_UploadQueue.push_back(page);
                }
            }
            // ── 清空本帧要用的环槽与戳记数组（等掉在飞命令之后，CPU 写是安全的；
            //     不需要 GPU 侧清屏 pass）──
            std::memset(p, 0, ((usize)kNanitePageFeedbackWords + m_Plan.pageCount) * sizeof(u32));
            buf->Unmap();
        }
    }

    // ── ② 限流上传：每帧最多 `uploadsPerFrame` 页，按请求到达顺序（FIFO）──
    // 【为什么用 FIFO 而不是"屏幕误差贡献"】后者需要 LOD 选择与逐簇误差，属阶段二
    //   （§14.32 ⑫ 明确不做）；阶段一的优先级就是"谁先被请求谁先来"，可复现、可核对。
    while (m_UploadQueueHead < m_UploadQueue.size() &&
           m_UploadsThisFrame < m_Config.uploadsPerFrame) {
        const u32 page = m_UploadQueue[m_UploadQueueHead++];
        m_Queued[page] = 0u;
        if (m_SlotOfPage[page] != kInvalidNanitePageSlot) continue;   // 期间已被别处装进来
        const u32 slot = AcquireSlot(frameIndex);
        if (slot == kInvalidNanitePageSlot) {
            // 池满且没有满足帧龄门槛的牺牲者 ⇒ 本帧到此为止；该页会在下一帧被重新请求
            //（队列里的其余页保持原序，不丢）
            --m_UploadQueueHead;
            m_Queued[page] = 1u;
            break;
        }
        UploadPage(page, slot);
        ++m_UploadsThisFrame;
    }
    if (m_UploadQueueHead >= m_UploadQueue.size()) {   // 队列排空 ⇒ 复位（避免无限增长）
        m_UploadQueue.clear();
        m_UploadQueueHead = 0u;
    }

    // ── ③ 重写页表（只在真的改过时；整表 = pageCount × 16B，Sponza 约 17 页 ⇒ 272B）──
    if (m_TableDirty != 0u) {
        if (void* p = m_PageTable->Map()) {
            std::memcpy(p, m_TableCPU.data(),
                        (usize)m_Plan.pageCount * sizeof(NanitePageTableEntry));
            m_PageTable->Unmap();
        }
        m_TableDirty = 0u;
    }
}

bool NaniteStream::PageTableSelfConsistent(u32& outResident, u32& outNonResident,
                                           u32& outDuplicateSlots) const {
    outResident = 0u;
    outNonResident = 0u;
    outDuplicateSlots = 0u;
    if (!m_Ready) return false;
    std::vector<u8> used(m_Config.poolSlots, 0u);
    for (u32 p = 0u; p < m_Plan.pageCount; ++p) {
        const u32 slot = m_SlotOfPage[p];
        if (slot == kInvalidNanitePageSlot) { ++outNonResident; continue; }
        ++outResident;
        if (slot >= m_Config.poolSlots) { ++outDuplicateSlots; continue; }   // 越界槽号 = 不自洽
        if (used[slot] != 0u) ++outDuplicateSlots;                            // 两页共用一槽
        used[slot] = 1u;
    }
    return (outResident + outNonResident == m_Plan.pageCount) && (outDuplicateSlots == 0u);
}

u32 NaniteStream::ReadbackGpuResidentPages() {
    if (!m_Ready || !m_PageTable) return 0u;
    u32 resident = 0u;
    if (void* p = m_PageTable->Map()) {
        const NanitePageTableEntry* table = static_cast<const NanitePageTableEntry*>(p);
        for (u32 page = 0u; page < m_Plan.pageCount; ++page) {
            if (table[page].resident != 0u) ++resident;
        }
        m_PageTable->Unmap();
    }
    return resident;
}

} // namespace he::render
