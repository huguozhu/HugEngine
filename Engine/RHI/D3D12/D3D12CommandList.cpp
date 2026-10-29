// ============================================================================
//  D3D12CommandList.cpp —— D3D12 命令录制的实现
// ============================================================================
#include "D3D12CommandList.h"
#include "D3D12Device.h"
#include "D3D12Resources.h"
#include "D3D12SwapChain.h"
#include "D3D12Converters.h"
#include "Core/CVar.h"   // r.Debug.DrawMarker（与 Vulkan 后端共用的逐绘制标签开关）
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>

#if defined(_WIN32)

namespace he::rhi::d3d12 {

D3D12CommandList::~D3D12CommandList() {
    if (!m_Device) return;
    m_Device->UnregisterCommandList(this);
    // 【GPU 可能仍在执行】RenderGraph 每帧新建异步计算列表，提交后函数返回即析构：此前原生列表、
    // 分配器、命令签名、临时资源随之立即释放，GPU 还在读它们（命令内存被回收 ⇒ 页错误 / 设备移除）。
    // 取各槽位最后一次提交的围栏值，未完成就交给设备延迟到围栏到达后再释放
    u64 last = 0;
    for (u64 v : m_SlotFenceValues) last = std::max(last, v);
    if (!m_SlotFence || last == 0 || m_SlotFence->GetCompletedValue() >= last) return;
    std::vector<ComPtr<IUnknown>> keep;
    if (m_List) keep.emplace_back(m_List);
    for (auto& a : m_Allocators) if (a) keep.emplace_back(a);
    for (auto& [key, sig] : m_CmdSigs) if (sig) keep.emplace_back(sig);
    for (auto& slot : m_TempResources) for (auto& r : slot) if (r) keep.emplace_back(r);
    m_Device->DeferRelease(m_SlotFence, last, std::move(keep));
}

bool D3D12CommandList::Initialize(D3D12Device* device, QueueType queue) {
    if (!device || !device->GetDevice()) return false;
    m_Device = device;
    ID3D12Device* dev = device->GetDevice();

    // 图形的 DIRECT 队列 / 计算的 COMPUTE 队列
    const D3D12_COMMAND_LIST_TYPE type =
        (queue == QueueType::Compute) ? D3D12_COMMAND_LIST_TYPE_COMPUTE
                                      : D3D12_COMMAND_LIST_TYPE_DIRECT;

    for (auto& alloc : m_Allocators) {
        if (!HE_D3D12_CHECK(dev->CreateCommandAllocator(type, IID_PPV_ARGS(&alloc)),
                            "CreateCommandAllocator")) {
            return false;
        }
    }
    m_Allocator = m_Allocators[0];
    if (!HE_D3D12_CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_SlotFence)),
                        "CreateFence(CommandList)")) {
        return false;
    }
    if (!HE_D3D12_CHECK(dev->CreateCommandList(0, type, m_Allocator.Get(), nullptr,
                                               IID_PPV_ARGS(&m_List)),
                        "CreateCommandList")) {
        return false;
    }
    m_Device->RegisterCommandList(this);   // DRED 报告据此把面包屑对应回事件名
    // 命名：DRED 报告与 RenderDoc 里据此区分图形 / 计算列表
    m_List->SetName(queue == QueueType::Compute ? L"HugEngine 计算命令列表" : L"HugEngine 图形命令列表");
    // 创建后默认是开着的，先关掉，等 Begin() 再 Reset
    m_List->Close();
    return true;
}

void D3D12CommandList::Begin() {
    if (!m_List || !m_Allocator) return;
    // 轮换到下一个帧槽位，并等待该槽位上一次提交的命令在 GPU 上执行完，再复用其分配器
    m_FrameSlot = (m_FrameSlot + 1) % kMaxFramesInFlight;
    m_Allocator = m_Allocators[m_FrameSlot];
    if (m_SlotFence && m_SlotFence->GetCompletedValue() < m_SlotFenceValues[m_FrameSlot]) {
        HANDLE ev = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        if (ev) {
            m_SlotFence->SetEventOnCompletion(m_SlotFenceValues[m_FrameSlot], ev);
            WaitForSingleObject(ev, 8000);   // 有上限：设备挂起时不至于让进程永久卡住
            CloseHandle(ev);
        }
    }
    // 该槽位上一轮的临时资源：GPU 已执行完（上面已等围栏），可以释放
    m_TempResources[m_FrameSlot].clear();
    // 每次录制前必须 Reset 分配器与列表
    if (FAILED(m_Allocator->Reset())) {
        // 【关键】分配器仍被 GPU 使用时 Reset 会失败。此前直接 return，
        // 列表停留在 Close 状态，之后所有录制（清屏、绘制、屏障）都被 D3D12 丢弃，
        // 调试层报 "This API cannot be called on a closed command list"，
        // 表现就是「引擎在出帧、几何也在提交，但画面全黑」。
        // 这里等 GPU 空闲后重试一次。
        HE_CORE_WARN("D3D12CommandList::Begin: 分配器 Reset 失败，等待 GPU 空闲后重试");
        if (m_Device) m_Device->WaitIdle();
        if (FAILED(m_Allocator->Reset())) {
            HE_CORE_ERROR("D3D12CommandList::Begin: 分配器 Reset 仍失败");
            return;
        }
    }
    if (FAILED(m_List->Reset(m_Allocator.Get(), nullptr))) {
        HE_CORE_ERROR("D3D12CommandList::Begin: 列表 Reset 失败");
        return;
    }
    m_Recording = true;
    // 新的录制：列表的根参数与描述符堆都已失效，清空缓存
    for (auto& t : m_RootTables) t.ptr = 0;
    m_RootArgsDirty = true;
    m_HeapsSet = false;
    for (auto& bs : m_BoundSets) bs = kInvalidSet;
    for (auto& ss : m_SnapSets) ss = kInvalidSet;   // 新的录制：快照一律重新拍
    m_LabelDepth   = 0;         // 标签层级与待插入的逐绘制标签不跨录制
    m_PostDrawBarriers.clear(); // 待补的绘制后屏障不跨录制
    m_EventLogs[m_FrameSlot].begins.clear();    // 本槽位上一次的录制已执行完，记录作废
    m_EventLogs[m_FrameSlot].markers.clear();
    m_DrawLabelSet = false;
    m_SetRingChunkMask = 0;
    m_PendingWaits.clear();     // 上一次录制若未提交，残留的等待 / 信号作废
    m_PendingSignals.clear();
    ClearPassAttachments();
    // 缓冲在每次提交执行完后衰减回 COMMON：新录制从全 COMMON 开始
    m_BufferStates.clear();
    // 为整帧申请一块推送常量槽位，并清零
    m_PushGpu = 0;
    m_PushCpu = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE dummyCbv{};
    if (m_Device && m_Device->AllocateConstants(m_PushGpu, dummyCbv)) {
        m_PushCpu = m_Device->GetConstantSlotPtr(m_PushGpu);
        if (m_PushCpu) std::memset(m_PushCpu, 0, 256);
    }
}

void D3D12CommandList::OnSubmitted(ID3D12CommandQueue* queue) {
    if (!queue || !m_SlotFence) return;
    const u64 v = m_NextFenceValue++;
    queue->Signal(m_SlotFence.Get(), v);
    m_SlotFenceValues[m_FrameSlot] = v;
    // 本次录制用过的描述符集快照块：围栏到达 v 前不能被覆盖
    if (m_Device && m_SetRingChunkMask) m_Device->MarkSetRingChunks(m_SetRingChunkMask, m_SlotFence.Get(), v);
    m_SetRingChunkMask = 0;
    // 本次提交写过的查询：围栏到达 v 后结果可读
    for (const auto& [pool, idx] : m_PendingQueries) pool->MarkSubmitted(idx, m_SlotFence.Get(), v);
    m_PendingQueries.clear();
}

// ── 调试标签 ──
// PIX 事件的 Metadata：0 = UTF-16 字符串（WINPIX_EVENT_UNICODE_VERSION），数据含结尾的 0。
// 不链接 WinPixEventRuntime，直接用 ID3D12GraphicsCommandList 的 BeginEvent / SetMarker。
namespace {
constexpr UINT kPixEventUnicode = 0;
}

void D3D12CommandList::BeginDebugLabel(const char* name, const float*) {
    if (!m_List || !m_Recording) return;
    const std::wstring w = Utf8ToWide(name ? name : "");
    m_List->BeginEvent(kPixEventUnicode, w.c_str(), static_cast<UINT>((w.size() + 1) * sizeof(wchar_t)));
    ++m_LabelDepth;
    if (DredEventLogEnabled()) m_EventLogs[m_FrameSlot].begins.emplace_back(name ? name : "");
}

void D3D12CommandList::EndDebugLabel() {
    if (!m_List || !m_Recording || m_LabelDepth == 0) return;   // 不配对的 End 直接忽略（D3D12 会判非法）
    m_List->EndEvent();
    --m_LabelDepth;
}

void D3D12CommandList::InsertDebugLabel(const char* name, const float*) {
    if (!m_List || !m_Recording || !name || !*name) return;
    const std::wstring w = Utf8ToWide(name);
    m_List->SetMarker(kPixEventUnicode, w.c_str(), static_cast<UINT>((w.size() + 1) * sizeof(wchar_t)));
    if (DredEventLogEnabled()) m_EventLogs[m_FrameSlot].markers.emplace_back(name);
}

bool D3D12CommandList::DredEventLogEnabled() {
    static const bool s_On = (std::getenv("HE_D3D12_DRED") != nullptr);
    return s_On;
}

bool D3D12CommandList::FindEventNames(const ID3D12CommandList* native, u32 beginCount, u32 markerCount,
                                      std::vector<std::string>& begins, std::vector<std::string>& markers) const {
    if (!native || native != static_cast<const ID3D12CommandList*>(m_List.Get())) return false;
    for (const auto& log : m_EventLogs) {
        if (log.begins.size() == beginCount && log.markers.size() == markerCount && (beginCount || markerCount)) {
            begins = log.begins;
            markers = log.markers;
            return true;
        }
    }
    return false;
}

void D3D12CommandList::SetDrawDebugLabel(const char* name, const float*) {
    // 空标签视为清除，避免残留到下一个绘制
    if (!name || !*name) { m_DrawLabelSet = false; return; }
    m_DrawLabel    = name;
    m_DrawLabelSet = true;
}

void D3D12CommandList::ClearDrawDebugLabel() {
    m_DrawLabelSet = false;
}

void D3D12CommandList::EmitDrawLabel() {
    // 一次性语义：插入后自动清除，防止跨 pass 泄漏脏标签。开关与 Vulkan 后端共用 r.Debug.DrawMarker
    if (!m_DrawLabelSet) return;
    m_DrawLabelSet = false;
    static const CVarBase* s_DrawMarker = FindCVar("r.Debug.DrawMarker");
    if (s_DrawMarker) {
        const CVarValue v = s_DrawMarker->GetValue();
        if (const bool* on = std::get_if<bool>(&v); on && !*on) return;
    }
    InsertDebugLabel(m_DrawLabel.c_str());
}

// ── GPU 查询（时间戳 / 管线统计）──
void D3D12CommandList::WriteTimestamp(IRHIQueryPool* pool, u32 queryIndex) {
    auto* qp = static_cast<D3D12QueryPool*>(pool);
    if (!m_List || !qp || queryIndex >= qp->GetQueryCount() || qp->GetQueryType() != QueryType::Timestamp) return;
    m_List->EndQuery(qp->GetHeap(), D3D12_QUERY_TYPE_TIMESTAMP, queryIndex);
    // 写完立刻解析到回读缓冲（READBACK 堆一直处于 COPY_DEST，无需屏障）
    m_List->ResolveQueryData(qp->GetHeap(), D3D12_QUERY_TYPE_TIMESTAMP, queryIndex, 1,
                             qp->GetReadback(), static_cast<UINT64>(queryIndex) * qp->GetResultStride());
    m_PendingQueries.emplace_back(qp, queryIndex);
}

void D3D12CommandList::BeginQuery(IRHIQueryPool* pool, u32 queryIndex) {
    auto* qp = static_cast<D3D12QueryPool*>(pool);
    if (!m_List || !qp || queryIndex >= qp->GetQueryCount() ||
        qp->GetQueryType() != QueryType::PipelineStatistics) return;
    m_List->BeginQuery(qp->GetHeap(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, queryIndex);
}

void D3D12CommandList::EndQuery(IRHIQueryPool* pool, u32 queryIndex) {
    auto* qp = static_cast<D3D12QueryPool*>(pool);
    if (!m_List || !qp || queryIndex >= qp->GetQueryCount() ||
        qp->GetQueryType() != QueryType::PipelineStatistics) return;
    m_List->EndQuery(qp->GetHeap(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, queryIndex);
    m_List->ResolveQueryData(qp->GetHeap(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, queryIndex, 1,
                             qp->GetReadback(), static_cast<UINT64>(queryIndex) * qp->GetResultStride());
    m_PendingQueries.emplace_back(qp, queryIndex);
}

void D3D12CommandList::ResetQueryPool(IRHIQueryPool* pool) {
    // D3D12 的查询不需要 GPU 端重置；只清 CPU 侧可用标记（对应 vkCmdResetQueryPool 后结果不可用）
    if (auto* qp = static_cast<D3D12QueryPool*>(pool)) qp->ResetAll();
}

void D3D12CommandList::GetQueryResults(IRHIQueryPool* pool, u32 first, u32 count, u64* data) {
    ReadQueryResults(pool, first, count, data, true);    // 对应 VK_QUERY_RESULT_WAIT_BIT
}

bool D3D12CommandList::TryGetQueryResults(IRHIQueryPool* pool, u32 first, u32 count, u64* data) {
    return ReadQueryResults(pool, first, count, data, false);   // 不等待，未就绪返回 false
}

bool D3D12CommandList::ReadQueryResults(IRHIQueryPool* pool, u32 first, u32 count, u64* data, bool wait) {
    auto* qp = static_cast<D3D12QueryPool*>(pool);
    if (!qp || !data || count == 0) return false;
    bool all = true;
    if (qp->GetQueryType() != QueryType::PipelineStatistics) {
        for (u32 i = 0; i < count; ++i) {
            const u32  q  = first + i;
            const bool ok = qp->IsAvailable(q, wait);
            u64 v = 0;
            if (ok) std::memcpy(&v, qp->ResultPtr(q), sizeof(v));
            data[i] = v;
            all = all && ok;
        }
        return all;
    }
    // 管线统计：与 Vulkan 查询池选的统计项同序，每个查询输出 6 个 u64：
    // IA 顶点、IA 图元、VS 调用、裁剪调用、裁剪图元、PS 调用。count 是查询个数（与 Vulkan 一致），data 容纳 count × 6 个 u64
    constexpr u32 kPerQuery = 6;
    for (u32 k = 0; k < count; ++k) {
        const u32  q  = first + k;
        const bool ok = qp->IsAvailable(q, wait);
        u64 vals[kPerQuery] = {};
        if (ok) {
            D3D12_QUERY_DATA_PIPELINE_STATISTICS st{};
            std::memcpy(&st, qp->ResultPtr(q), sizeof(st));
            vals[0] = st.IAVertices;   vals[1] = st.IAPrimitives; vals[2] = st.VSInvocations;
            vals[3] = st.CInvocations; vals[4] = st.CPrimitives;  vals[5] = st.PSInvocations;
        }
        all = all && ok;
        for (u32 j = 0; j < kPerQuery; ++j) data[k * kPerQuery + j] = vals[j];
    }
    return all;
}

void D3D12CommandList::PrepareForExternalRendering() {
    if (!m_List || !m_Device || !m_Device->EnsureDescriptorHeaps()) return;
    ID3D12DescriptorHeap* heaps[2] = { m_Device->GetCbvSrvUavHeap(), m_Device->GetSetSamplerHeap() };
    if (heaps[0] && heaps[1]) {
        m_List->SetDescriptorHeaps(2, heaps);
        m_HeapsSet = true;
    }
    m_Pipeline = nullptr;      // 外部会设置自己的 PSO / 根签名
    m_RootArgsDirty = true;
}

void D3D12CommandList::End() {
    if (TraceEnabled()) { std::printf("[TRACE] End\n"); std::fflush(stdout); }

    if (!m_List || !m_Recording) return;
    // 引擎若漏调 EndDebugLabel，未闭合的事件会让 Close 失败：这里补齐
    for (; m_LabelDepth > 0; --m_LabelDepth) m_List->EndEvent();
    if (FAILED(m_List->Close())) {
        HE_CORE_ERROR("D3D12CommandList::End: Close 失败");
    }
    m_Recording = false;
}

void D3D12CommandList::BeginRenderPass(u32 colorCount, Format colorFormat,
                                       Format depthFormat, const ClearValue* clear,
                                       LoadOp loadOp) {
    if (!m_List) return;

    // 后缓冲的 RTV：从交换链取（引擎传的 colorFormat 是后缓冲格式）
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    bool hasRtv = false;
    // 【兜底】渲染线程创建的命令列表未必带 m_SwapChain；此时若不兜底，
    // hasRtv 会是 false ⇒ 后缓冲根本没被绑定 ⇒ 最终画面永远黑，
    // 且后缓冲状态停在 PRESENT/COMMON ⇒ 截图时的屏障与拷贝也无效。
    if (!m_SwapChain && m_Device && m_Device->GetSwapChainPtr()) {
        m_SwapChain = m_Device->GetSwapChainPtr();
    }
    if (m_SwapChain) {
        // 引擎的 IRHISwapChain 只有 D3D12 交换链会走到这里；
        // 用 static_cast 而不是 dynamic_cast，避免依赖 RTTI。
        auto* d3dSc = static_cast<D3D12SwapChain*>(m_SwapChain);
        rtv = d3dSc->GetCurrentRtv();
        hasRtv = true;
    }

    // 【交换链后缓冲必须显式屏障】D3D12 不会把 PRESENT 隐式提升为 RENDER_TARGET，
    // 不做转换就以 RTV 使用会被判非法、渲染内容被丢弃（调试层 id=538 实测 1669 条，
    // 这也是 02.Cube 画面全黑的根因）。
    if (hasRtv && m_SwapChain) {
        auto* sc = static_cast<D3D12SwapChain*>(m_SwapChain);
        ID3D12Resource* bb = sc->GetCurrentBackBufferResource();
        if (bb && sc->GetBackBufferState() != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            D3D12_RESOURCE_BARRIER tb{};
            tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            tb.Transition.pResource   = bb;
            tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            tb.Transition.StateBefore = sc->GetBackBufferState();
            tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            m_List->ResourceBarrier(1, &tb);
        }
    }

    m_List->OMSetRenderTargets(colorCount, hasRtv ? &rtv : nullptr, FALSE, nullptr);
    m_HasRtv = hasRtv;
    // 告诉交换链：后缓冲现在处于 RENDER_TARGET 状态（截图做屏障时要用真实状态）
    if (hasRtv && m_SwapChain) {
        static_cast<D3D12SwapChain*>(m_SwapChain)->MarkBackBufferRendered();
    }

    if (hasRtv && loadOp == LoadOp::Clear) {
        const float c[4] = {
            clear ? clear->color[0] : 0.0f,
            clear ? clear->color[1] : 0.0f,
            clear ? clear->color[2] : 0.0f,
            clear ? clear->color[3] : 1.0f,
        };
        m_List->ClearRenderTargetView(rtv, c, 0, nullptr);
    }

    // 视口与裁剪默认铺满整个后缓冲
    if (m_SwapChain) {
        D3D12_VIEWPORT vp{};
        vp.Width  = static_cast<float>(m_SwapChain->GetWidth());
        vp.Height = static_cast<float>(m_SwapChain->GetHeight());
        vp.MaxDepth = 1.0f;
        m_List->RSSetViewports(1, &vp);
        D3D12_RECT rc{ 0, 0,
                       static_cast<LONG>(m_SwapChain->GetWidth()),
                       static_cast<LONG>(m_SwapChain->GetHeight()) };
        m_List->RSSetScissorRects(1, &rc);
    }
    (void)depthFormat;
}

void D3D12CommandList::EndRenderPass() {
    if (TraceEnabled()) { std::printf("[TRACE] EndRenderPass\n"); std::fflush(stdout); }

    // D3D12 没有 pass 结束的概念；资源状态转换由显式 PipelineBarrier 负责
    ClearPassAttachments();
}

void D3D12CommandList::BeginOffscreenPass(void* colorImageView, void* depthImageView,
                                          u32 width, u32 height,
                                          const ClearValue* clear,
                                          bool allowSecondary) {
    (void)allowSecondary;
    if (!m_List) return;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ToCpuHandle(colorImageView);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ToCpuHandle(depthImageView);
    const bool hasRtv = (colorImageView != nullptr);
    const bool hasDsv = (depthImageView != nullptr);
    // 记下本 pass 的附件：绘制前补纹理状态时跳过它们
    SetPassAttachments(&rtv, hasRtv ? 1u : 0u, hasDsv ? &dsv : nullptr);

    // RTV 数量必须与指针一致：只有深度附件的 pass 传 1+nullptr 是非法调用（设备会被移除）
    // 【必须做资源屏障】D3D12 要求绑定为 RTV/DSV 的资源处于 RENDER_TARGET /
    // DEPTH_WRITE 状态。引擎只把 CPU 句柄传进来，所以先按句柄反查资源，
    // 再按记录的状态做一次转换（否则 OMSetRenderTargets / Clear 会静默卡死）。
    if (m_Device) {
        if (hasRtv) {
            if (auto* res = m_Device->FindViewResource(static_cast<UINT64>(rtv.ptr))) {
                const auto from = m_Device->GetResourceState(res);
                // 【只要不是目标状态就显式转换】此前只在「当前是着色器资源」时才转，
                // 从 COMMON 依赖所谓隐式提升 —— 但调试层明确判定 COMMON 不能直接当
                // 渲染目标使用（id=538 实测 1118 条），渲染内容被丢弃。
                if (from != D3D12_RESOURCE_STATE_RENDER_TARGET) {
                    D3D12_RESOURCE_BARRIER b{};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Transition.pResource = res;
                    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    b.Transition.StateBefore = from;
                    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    m_List->ResourceBarrier(1, &b);
                    m_Device->SetResourceState(res, D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
            }
        }
        if (hasDsv) {
            if (auto* res = m_Device->FindViewResource(static_cast<UINT64>(dsv.ptr))) {
                const auto from = m_Device->GetResourceState(res);
                if (from != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
                    D3D12_RESOURCE_BARRIER b{};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Transition.pResource = res;
                    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    b.Transition.StateBefore = from;
                    b.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                    m_List->ResourceBarrier(1, &b);
                    m_Device->SetResourceState(res, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                }
            }
        }
    }

    // 【HDR 中间结果转储】进入宽屏离屏 pass 时，把该目标回读并打印首像素。
    // 此刻里面是**上一帧**完整渲染后的结果，可判断几何是否真的画进了 HDR。
    // 【任意尺寸的渲染目标回读扫描】HE_DUMP_HDR=1 时，把每个离屏 pass 的目标
    // 拷到回读缓冲，下一帧等 GPU 完成后扫描整张图，统计非零像素与最大值。
    // 用途：定位"哪个 pass 真正产出了内容"，从而找出黑屏发生在哪一步。
    static const bool s_DumpHdr = (std::getenv("HE_DUMP_HDR") != nullptr);   // 只读一次环境变量
    if (s_DumpHdr && hasRtv && m_Device && m_Device->GetDevice() && m_Device->GetQueue()) {
        if (ID3D12Resource* hres = m_Device->FindViewResource(static_cast<UINT64>(rtv.ptr))) {
            const D3D12_RESOURCE_DESC hd = hres->GetDesc();
            const u32 bpp = 8;                       // 只处理 16F/RGBA 类
            const u32 rowPitch = static_cast<u32>((hd.Width * bpp + 255) & ~255ull);
            const u64 need = static_cast<u64>(rowPitch) * hd.Height;
            static ComPtr<ID3D12Resource> s_rb;
            static int s_pending = 0;
            static u64 s_key = 0;
            const u64 key = (static_cast<u64>(hd.Width) << 32) | static_cast<u64>((u32)hd.Format);
            if (!s_rb || s_key != key) {
                s_rb.Reset();
                D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                rd.Width = need; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
                rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc = {1,0};
                rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                m_Device->GetDevice()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s_rb));
                s_key = key; s_pending = 0;
            }
            if (s_rb && s_pending) {
                m_Device->WaitIdle();
                void* mp = nullptr; D3D12_RANGE rr{0,0};
                if (SUCCEEDED(s_rb->Map(0, &rr, &mp)) && mp) {
                    const unsigned char* raw = static_cast<const unsigned char*>(mp);
                    u64 nonZero = 0, total = 0; float mx = 0.0f;
                    for (u32 yy = 0; yy < static_cast<u32>(hd.Height); yy += 8) {
                        const float* row = reinterpret_cast<const float*>(raw + static_cast<size_t>(yy) * rowPitch);
                        for (u32 xx = 0; xx < static_cast<u32>(hd.Width); xx += 8) {
                            const float r0 = row[xx*4+0], g0 = row[xx*4+1], b0 = row[xx*4+2];
                            ++total;
                            if (r0 != 0.0f || g0 != 0.0f || b0 != 0.0f) ++nonZero;
                            if (r0 > mx) mx = r0; if (g0 > mx) mx = g0; if (b0 > mx) mx = b0;
                        }
                    }
                    HE_CORE_ERROR("[RT-SCAN] {}x{} fmt={} 采样={} 非零={} max={}",
                        static_cast<u64>(hd.Width), static_cast<u32>(hd.Height),
                        static_cast<u32>(hd.Format), static_cast<u64>(total),
                        static_cast<u64>(nonZero), mx);
                    s_rb->Unmap(0, nullptr);
                }
                s_pending = 0;
            }
            if (s_rb) {
                D3D12_RESOURCE_BARRIER bb{};
                bb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; bb.Transition.pResource = hres;
                bb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                bb.Transition.StateBefore = m_Device->GetResourceState(hres);
                bb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
                if (bb.Transition.StateBefore != bb.Transition.StateAfter) m_List->ResourceBarrier(1, &bb);
                D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = s_rb.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint.Footprint.Format = hd.Format;
                dst.PlacedFootprint.Footprint.Width = static_cast<UINT>(hd.Width);
                dst.PlacedFootprint.Footprint.Height = hd.Height;
                dst.PlacedFootprint.Footprint.Depth = 1;
                dst.PlacedFootprint.Footprint.RowPitch = rowPitch;
                D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = hres;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
                m_List->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                D3D12_RESOURCE_BARRIER rb = bb;
                rb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                rb.Transition.StateAfter  = m_Device->GetResourceState(hres);
                if (rb.Transition.StateBefore != rb.Transition.StateAfter) m_List->ResourceBarrier(1, &rb);
                s_pending = 1;
            }
        }
    }

    m_List->OMSetRenderTargets(hasRtv ? 1u : 0u, hasRtv ? &rtv : nullptr, FALSE, hasDsv ? &dsv : nullptr);
    m_HasRtv = hasRtv;

    // 【是否清空附件由 PSO 的 loadOp 决定】与 Vulkan 一致：离屏 pass 用当前 PSO 的 render pass，
    // Clear 才清空、Load 保留；clear 只提供清屏值（为空时颜色 0、深度 1.0）。此前深度无条件清空，
    // 延迟管线的天空盒 pass（depthLoadOp=Load）因此丢掉 GBuffer 深度、覆盖整张光照结果。
    const bool gfxPso     = m_Pipeline && !m_IsComputePipeline;
    const bool clearColor = gfxPso ? (m_Pipeline->GetColorLoadOp() == LoadOp::Clear) : (clear != nullptr);
    const bool clearDepth = gfxPso ? (m_Pipeline->GetDepthLoadOp() == LoadOp::Clear) : (clear != nullptr);
    static const float kZeroColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (hasRtv && clearColor) {
        m_List->ClearRenderTargetView(rtv, clear ? clear->color : kZeroColor, 0, nullptr);
    }
    if (hasDsv && clearDepth) {
        // 【只有带模板的格式才能传 STENCIL 标志】D32_FLOAT 没有模板位，
        // 同时传 DEPTH|STENCIL 会让整个 ClearDepthStencilView 被拒，
        // 深度缓冲留在未初始化状态 ⇒ 所有物体（含天空盒）都过不了深度测试 ⇒ 画面全黑。
        D3D12_CLEAR_FLAGS dflags = D3D12_CLEAR_FLAG_DEPTH;
        if (m_Device) {
            if (ID3D12Resource* dres = m_Device->FindViewResource(static_cast<UINT64>(dsv.ptr))) {
                const DXGI_FORMAT df = dres->GetDesc().Format;
                if (df == DXGI_FORMAT_D24_UNORM_S8_UINT || df == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
                    dflags |= D3D12_CLEAR_FLAG_STENCIL;
            }
        }
        m_List->ClearDepthStencilView(dsv, dflags,
                                       clear ? clear->depth : 1.0f,
                                       clear ? clear->stencil : 0, 0, nullptr);
    }

    D3D12_VIEWPORT vp{};
    vp.Width  = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MaxDepth = 1.0f;
    m_List->RSSetViewports(1, &vp);
    D3D12_RECT rc{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    m_List->RSSetScissorRects(1, &rc);
}

void D3D12CommandList::BeginOffscreenPassMRT(void* const* colorImageViews, u32 colorCount,
                                             void* depthImageView, u32 width, u32 height,
                                             const ClearValue* clears,
                                             bool allowSecondary) {
    (void)allowSecondary;
    if (!m_List || colorCount == 0) return;

    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8]{};
    const u32 n = (colorCount < 8) ? colorCount : 8;
    for (u32 i = 0; i < n; ++i) rtvs[i] = ToCpuHandle(colorImageViews[i]);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ToCpuHandle(depthImageView);
    const bool hasDsv = (depthImageView != nullptr);
    // 记下本 pass 的附件：绘制前补纹理状态时跳过它们
    SetPassAttachments(rtvs, n, hasDsv ? &dsv : nullptr);

    // 【MRT 也必须做状态转换】此前只有单目标路径做了屏障，多目标 pass（GBuffer 等）
    // 的渲染目标停在 COMMON/PRESENT，被调试层判定非法、绘制被丢弃（id=538）。
    for (u32 i = 0; i < n; ++i) {
        if (!colorImageViews[i] || !m_Device) continue;
        if (ID3D12Resource* res = m_Device->FindViewResource(static_cast<UINT64>(rtvs[i].ptr))) {
            const auto from = m_Device->GetResourceState(res);
            if (from != D3D12_RESOURCE_STATE_RENDER_TARGET) {
                D3D12_RESOURCE_BARRIER tb{};
                tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                tb.Transition.pResource = res;
                tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                tb.Transition.StateBefore = from;
                tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
                m_List->ResourceBarrier(1, &tb);
                m_Device->SetResourceState(res, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
        }
    }
    if (hasDsv && m_Device) {
        if (ID3D12Resource* dres = m_Device->FindViewResource(static_cast<UINT64>(dsv.ptr))) {
            const auto from = m_Device->GetResourceState(dres);
            if (from != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
                D3D12_RESOURCE_BARRIER tb{};
                tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                tb.Transition.pResource = dres;
                tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                tb.Transition.StateBefore = from;
                tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                m_List->ResourceBarrier(1, &tb);
                m_Device->SetResourceState(dres, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            }
        }
    }
    m_List->OMSetRenderTargets(n, rtvs, FALSE, hasDsv ? &dsv : nullptr);

    // 是否清空由 PSO 的 loadOp 决定（同 BeginOffscreenPass）
    const bool gfxPso     = m_Pipeline && !m_IsComputePipeline;
    const bool clearColor = gfxPso ? (m_Pipeline->GetColorLoadOp() == LoadOp::Clear) : (clears != nullptr);
    const bool clearDepth = gfxPso ? (m_Pipeline->GetDepthLoadOp() == LoadOp::Clear) : (clears != nullptr);
    static const float kZeroColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (clearColor) {
        for (u32 i = 0; i < n; ++i) {
            if (colorImageViews[i]) m_List->ClearRenderTargetView(rtvs[i], clears ? clears[i].color : kZeroColor, 0, nullptr);
        }
    }
    if (hasDsv && clearDepth) {
        // 【只有带模板的格式才能传 STENCIL 标志】D32_FLOAT 没有模板位，
        // 同时传 DEPTH|STENCIL 会让整个 ClearDepthStencilView 被拒，
        // 深度缓冲留在未初始化状态 ⇒ 所有物体（含天空盒）都过不了深度测试 ⇒ 画面全黑。
        D3D12_CLEAR_FLAGS dflags = D3D12_CLEAR_FLAG_DEPTH;
        if (m_Device) {
            if (ID3D12Resource* dres = m_Device->FindViewResource(static_cast<UINT64>(dsv.ptr))) {
                const DXGI_FORMAT df = dres->GetDesc().Format;
                if (df == DXGI_FORMAT_D24_UNORM_S8_UINT || df == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
                    dflags |= D3D12_CLEAR_FLAG_STENCIL;
            }
        }
        m_List->ClearDepthStencilView(dsv, dflags,
                                       clears ? clears[0].depth : 1.0f,
                                       clears ? clears[0].stencil : 0, 0, nullptr);
    }

    D3D12_VIEWPORT vp{};
    vp.Width  = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MaxDepth = 1.0f;
    m_List->RSSetViewports(1, &vp);
    D3D12_RECT rc{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    m_List->RSSetScissorRects(1, &rc);
}

void D3D12CommandList::EndOffscreenPass() {
    ClearPassAttachments();   // pass 结束：附件之后可以被转成可读状态
}

void D3D12CommandList::SetPassAttachments(const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, u32 rtvCount,
                                          const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    ClearPassAttachments();
    m_PassRtvCount = (rtvCount < 8u) ? rtvCount : 8u;
    for (u32 i = 0; i < m_PassRtvCount; ++i) {
        m_PassRtvs[i] = rtvs[i];
        if (m_Device && rtvs[i].ptr)
            if (auto* r = m_Device->FindViewResource(static_cast<UINT64>(rtvs[i].ptr))) m_PassAttachments.push_back(r);
    }
    if (dsv && dsv->ptr) {
        m_PassDsv = *dsv;
        m_PassDsvRes = m_Device ? m_Device->FindViewResource(static_cast<UINT64>(dsv->ptr)) : nullptr;
        if (m_PassDsvRes) m_PassAttachments.push_back(m_PassDsvRes);
    }
}

void D3D12CommandList::ClearPassAttachments() {
    m_PassAttachments.clear();
    m_PassRtvCount = 0;
    m_PassDsv = {};
    m_PassDsvRes = nullptr;
    m_DsvReadOnly = false;
}

void D3D12CommandList::SetPipeline(IRHIPipelineState* pso) {

    m_Pipeline = static_cast<D3D12PipelineState*>(pso);
    if (!m_List || !m_Pipeline) return;
    m_IsComputePipeline = m_Pipeline->IsCompute();
    // 【未在录制时只记状态】各样例初始化时会在命令列表 Begin 之前调一次 SetPipeline，
    // 而 D3D12 的列表创建后处于关闭状态：原生调用全部被拒绝（调试层 id=547），本就不生效。
    // 录制开始后每次绘制前都会重新 SetPipeline，这里跳过原生调用即可
    if (!m_Recording) {
        m_RootArgsDirty = true;
        return;
    }
    m_List->SetPipelineState(m_Pipeline->GetPso());
    // 【图形/计算根签名是两套独立绑定】计算管线必须用 SetComputeRootSignature，
    // 否则后面 SetComputeRootDescriptorTable 没有根签名可用，D3D12 会静默卡死。
    if (m_IsComputePipeline) {
        m_List->SetComputeRootSignature(m_Pipeline->GetRootSignature());
    } else {
        m_List->SetGraphicsRootSignature(m_Pipeline->GetRootSignature());
    }
    m_List->IASetPrimitiveTopology(m_Pipeline->GetTopology());
    // 引擎布局缺失的属性挂在零缓冲槽上，这里把零缓冲绑上
    if (m_Pipeline->NeedsZeroVertexBuffer() && m_Device) {
        const D3D12_VERTEX_BUFFER_VIEW zv = m_Device->GetZeroVertexBufferView();
        if (zv.BufferLocation) m_List->IASetVertexBuffers(D3D12PipelineState::kZeroVertexSlot, 1, &zv);
    }
    // 顶点着色器读实例号：绑定实例序号缓冲（读出值含起始实例，见 kInstanceIndexSlot）
    if (m_Pipeline->NeedsInstanceIndexBuffer() && m_Device) {
        const D3D12_VERTEX_BUFFER_VIEW iv = m_Device->GetInstanceIndexBufferView();
        if (iv.BufferLocation) m_List->IASetVertexBuffers(D3D12PipelineState::kInstanceIndexSlot, 1, &iv);
    }
    // 设置根签名会清空所有根参数，下一次绘制/派发前需要重新下发
    m_RootArgsDirty = true;
}

void D3D12CommandList::SetVertexBuffer(IRHIBuffer* buffer, u32 binding) {
    if (TraceEnabled()) { std::printf("[TRACE] SetVertexBuffer\n"); std::fflush(stdout); }
    if (!m_List || !buffer) return;
    auto* b = static_cast<D3D12Buffer*>(buffer);
    // 【关键】StrideInBytes 必须非零：D3D12 的 IASetVertexBuffers 收到 0 步长时
    // 会静默卡住（实测 01.Triangle 就卡在这个调用里，CPU 0%、无错误日志）。
    // 引擎的 Buffer 不一定填 stride ⇒ 用当前 PSO 从输入布局算出的步长兜底。
    u32 stride = static_cast<u32>(b->GetStride());
    if (stride == 0 && m_Pipeline) stride = m_Pipeline->GetVertexStride();
    if (stride == 0) {
        HE_CORE_WARN("D3D12 SetVertexBuffer: 步长为 0（buffer 与 PSO 都没提供），跳过绑定");
        return;
    }
    D3D12_VERTEX_BUFFER_VIEW vbv = b->GetVbv(stride);
    m_List->IASetVertexBuffers(binding, 1, &vbv);
}

void D3D12CommandList::SetIndexBuffer(IRHIBuffer* buffer, u32 offset) {
    if (TraceEnabled()) { std::printf("[TRACE] SetIndexBuffer\n"); std::fflush(stdout); }
    if (!m_List || !buffer) return;
    auto* b = static_cast<D3D12Buffer*>(buffer);
    D3D12_INDEX_BUFFER_VIEW ibv = b->GetIbv();
    ibv.BufferLocation += offset;
    m_List->IASetIndexBuffer(&ibv);
}

void D3D12CommandList::SetViewport(const Viewport& vp) {
    if (TraceEnabled()) { std::printf("[TRACE] SetViewport\n"); std::fflush(stdout); }
    if (!m_List) return;
    D3D12_VIEWPORT v{};
    v.TopLeftX = vp.x;
    v.TopLeftY = vp.y;
    v.Width    = vp.width;
    v.Height   = vp.height;
    v.MinDepth = vp.minDepth;
    v.MaxDepth = vp.maxDepth;
    // D3D12 的 Y 轴向下；引擎传负高度表示「原点在左下」，这里换算成 D3D12 的原点左上
    if (vp.height < 0.0f) {
        v.TopLeftY = vp.y + vp.height;
        v.Height   = -vp.height;
    }
    m_LastVp = v;
    m_List->RSSetViewports(1, &v);
}

void D3D12CommandList::SetScissor(const ScissorRect& sc) {
    if (TraceEnabled()) { std::printf("[TRACE] SetScissor\n"); std::fflush(stdout); }
    if (!m_List) return;
    D3D12_RECT r{};
    r.left   = sc.x;
    r.top    = sc.y;
    r.right  = sc.x + static_cast<i32>(sc.width);
    r.bottom = sc.y + static_cast<i32>(sc.height);
    m_List->RSSetScissorRects(1, &r);
}

void D3D12CommandList::SetFullscreenViewport(u32 width, u32 height) {
    Viewport vp{};
    vp.width  = static_cast<float>(width);
    vp.height = static_cast<float>(height);
    SetViewport(vp);
    ScissorRect sc{};
    sc.width  = width;
    sc.height = height;
    SetScissor(sc);
}

void D3D12CommandList::BindDescriptorSet(u32 setIndex, DescriptorSetHandle set) {
    if (!m_List || !m_Device || set == kInvalidSet || setIndex >= 8) return;
    if (!m_Device->EnsureDescriptorHeaps()) return;
    m_Device->FlushPendingBindlessBuffers();   // 补写初始化早期被丢弃的 bindless 缓冲描述符

    ID3D12DescriptorHeap* heaps[2] = { m_Device->GetCbvSrvUavHeap(), m_Device->GetSetSamplerHeap() };
    if (!heaps[0] || !heaps[1]) return;
    m_List->SetDescriptorHeaps(2, heaps);
    m_HeapsSet = true;

    m_BoundSets[setIndex] = set;
    const u32 baseSlot = m_Device->GetSetBaseSlot(set);
    m_LastBoundSet = set; m_LastBaseSlot = baseSlot;
    if (baseSlot == 0xFFFFFFFFu) return;

    // 【参数偏移必须与根签名一致】根签名布局：8 个 root CBV 之后依次是
    //   SRV 表 8..15、UAV 表 16..23、Sampler 表 24..31（每类 8 个寄存器空间）。
    // 之前这里用 12/16 是老布局（当时 kSpaceCount=4）的偏移，把 UAV/Sampler
    // 表绑到了 SRV 的槽位上，描述符类型对不上会让 D3D12 直接卡死。
    //
    // SRV / UAV 表不在这里设置：它们指向集内容的快照，由 PrepareSetTables 在绘制/派发前
    // 按集的当前版本拍摄（与 Vulkan UPDATE_AFTER_BIND 一致：绑定之后的更新对之后的绘制可见）。
    // 采样器表从该 set 自己的采样器基址开始（见 AllocateDescriptorSet）；
    // 只记录到缓存，由 FlushRootArgs 在绘制/派发前按当前管线类型（图形/计算）下发
    SetRootTable(24 + setIndex, m_Device->GetSetSamplerGpuHandle(m_Device->GetSetSamplerBase(set)));
    m_RootArgsDirty = true;
}

void D3D12CommandList::PrepareSetTables() {
    if (!m_Device) return;
    // space2 的 bindless 表：此前只在 DrawIndexed 里设置，计算派发与 Draw 都没有，
    // 用 bindless 的 compute（如逐实例剔除）读到未初始化的根参数（GPU 验证 id=935）
    if (const auto h = m_Device->GetBindlessSrvGpuHandle(); h.ptr) SetRootTable(10, h);
    if (const auto h = m_Device->GetBindlessUavGpuHandle(); h.ptr) SetRootTable(18, h);
    if (const auto h = m_Device->GetBindlessSmpGpuHandle(); h.ptr) SetRootTable(26, h);
    for (u32 i = 0; i < 8u; ++i) {
        const DescriptorSetHandle set = m_BoundSets[i];
        if (set == kInvalidSet) continue;
        const u64 ver = m_Device->GetSetVersion(set);
        if (m_SnapSets[i] == set && m_SnapVersions[i] == ver) continue;   // 快照仍有效
        D3D12_GPU_DESCRIPTOR_HANDLE srv{}, uav{};
        u32 chunk = 0;
        if (!m_Device->SnapshotDescriptorSet(set, srv, uav, chunk)) continue;
        SetRootTable(8  + i, srv);
        SetRootTable(16 + i, uav);
        m_SnapSets[i]     = set;
        m_SnapVersions[i] = ver;
        m_SetRingChunkMask |= (1u << chunk);
    }
}

void D3D12CommandList::AfterDrawOrDispatch() {
    if (m_PostDrawBarriers.empty() || !m_List) return;
    m_List->ResourceBarrier(static_cast<UINT>(m_PostDrawBarriers.size()), m_PostDrawBarriers.data());
    m_PostDrawBarriers.clear();
}

void D3D12CommandList::EnsureBoundResourceStates() {
    if (!m_List || !m_Device || !m_Pipeline) return;
    constexpr D3D12_RESOURCE_STATES kRead =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    m_BarrierScratch.clear();
    const bool gfxPso = !m_IsComputePipeline;

    // 当前绑着只读 DSV，但新的 PSO 要写深度：先切回可写 DSV
    if (m_DsvReadOnly && gfxPso && m_PassDsvRes && m_Pipeline->GetDepthWrite()) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = m_PassDsvRes;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = m_Device->GetResourceState(m_PassDsvRes);
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        m_List->ResourceBarrier(1, &b);
        m_Device->SetResourceState(m_PassDsvRes, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        m_List->OMSetRenderTargets(m_PassRtvCount, m_PassRtvCount ? m_PassRtvs : nullptr, FALSE, &m_PassDsv);
        m_DsvReadOnly = false;
    }
    bool needReadOnlyDsv = false;

    // 【同一纹理：整体读 + 某一级写】Hi-Z 逐级下采样在一次派发里把整张金字塔当 SRV 读（第 N 级），
    // 同时经逐 mip 视图把第 N+1 级当 UAV 写。整资源状态只能取其一（此前按先出现的用途转换，属非法使用）。
    // 这里按子资源拆开：整张先转到可读，只把被写的那一级转到 UAV；派发后再把它转回可读
    //（见 AfterDrawOrDispatch），对外仍是「整张纹理处于同一状态」，其他代码无需感知子资源。
    std::vector<ID3D12Resource*> splitHandled;
    // 【同一纹理：整体读 + 整体写】如 Lumen 全局 SDF 的层集把同一张 3D 纹理既绑成 UAV（转换阶段写）
    // 又绑成带采样器的 SRV（占位）。此前按先出现的用途转换：SRV 在前时纹理停在只读状态，
    // UAV 写入非法、在 Intel 上直接丢失（实测全局 SDF 自检 FAIL）。这种组合以写为准，读用途跳过
    std::vector<ID3D12Resource*> uavWins;
    {
        struct Usage { bool read = false; bool uavWhole = false; UINT uavSub = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; };
        std::vector<std::pair<ID3D12Resource*, Usage>> usages;
        for (const auto& use : m_Pipeline->GetResourceUses()) {
            if (use.space >= 8u || m_BoundSets[use.space] == kInvalidSet) continue;
            bool isBuffer = false;
            UINT sub = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ID3D12Resource* res = m_Device->GetSetResource(m_BoundSets[use.space], use.binding, isBuffer, &sub);
            if (!res || isBuffer) continue;
            auto it = std::find_if(usages.begin(), usages.end(), [&](const auto& u) { return u.first == res; });
            if (it == usages.end()) { usages.emplace_back(res, Usage{}); it = usages.end() - 1; }
            if (!use.uav) it->second.read = true;
            else if (sub == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) it->second.uavWhole = true;
            else it->second.uavSub = sub;
        }
        for (const auto& [res, u] : usages) {
            if (u.read && u.uavWhole) { uavWins.push_back(res); continue; }
            if (!u.read || u.uavWhole || u.uavSub == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) continue;
            if (std::find(m_PassAttachments.begin(), m_PassAttachments.end(), res) != m_PassAttachments.end()) continue;
            const D3D12_RESOURCE_STATES cur = m_Device->GetResourceState(res);
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = res;
            if (cur != kRead) {   // 整张 → 可读
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = cur;
                b.Transition.StateAfter  = kRead;
                m_BarrierScratch.push_back(b);
            }
            b.Transition.Subresource = u.uavSub;   // 被写的那一级 → UAV
            b.Transition.StateBefore = kRead;
            b.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            m_BarrierScratch.push_back(b);
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);   // 绘制后转回可读
            m_PostDrawBarriers.push_back(b);
            m_Device->SetResourceState(res, kRead);   // 绘制后整张统一为可读
            splitHandled.push_back(res);
        }
    }

    for (const auto& use : m_Pipeline->GetResourceUses()) {
        if (use.space >= 8u || m_BoundSets[use.space] == kInvalidSet) continue;
        bool isBuffer = false;
        ID3D12Resource* res = m_Device->GetSetResource(m_BoundSets[use.space], use.binding, isBuffer);
        if (!res) continue;
        if (std::find(splitHandled.begin(), splitHandled.end(), res) != splitHandled.end()) continue;   // 已按子资源处理
        if (!use.uav && std::find(uavWins.begin(), uavWins.end(), res) != uavWins.end()) continue;    // 同时被写：以写为准
        // 正作为本 pass 附件的资源不能同时转成可读（引擎真这样用时属于 feedback，保持原状）
        if (std::find(m_PassAttachments.begin(), m_PassAttachments.end(), res) != m_PassAttachments.end()) {
            // 深度附件被同时采样（如软粒子）且当前 PSO 不写深度：改绑只读 DSV，
            // 深度转到 DEPTH_READ | 着色器可读，对应 Vulkan 的 DEPTH_STENCIL_READ_ONLY 布局
            if (!use.uav && gfxPso && res == m_PassDsvRes && !m_Pipeline->GetDepthWrite()) needReadOnlyDsv = true;
            continue;
        }
        // 缓冲按本列表的记录（起始为 COMMON），纹理按设备级记录（纹理不会衰减）
        const D3D12_RESOURCE_STATES cur  = isBuffer ? GetBufferState(res) : m_Device->GetResourceState(res);
        const D3D12_RESOURCE_STATES want = use.uav ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : kRead;
        if (cur == want || (!use.uav && (cur & kRead) == kRead)) continue;   // 已满足（含额外的只读位）
        // 【缓冲首次使用走隐式提升】缓冲在每个列表开头都是 COMMON，且可以从 COMMON 隐式提升到
        // 任意状态（首次访问时自动完成）。这里不发显式屏障、只记下提升后的状态；同一列表里
        // 后续换用途时再从该状态显式转换。显式发 COMMON → X 会与验证层对实际状态的判断冲突
        //（GPU 验证 id=942 StateBefore invalid）。
        if (isBuffer && cur == D3D12_RESOURCE_STATE_COMMON) { m_BufferStates[res] = want; continue; }
        // 同一资源在一次绘制里只转一次（以先出现的用途为准）
        bool dup = false;
        for (const auto& bb : m_BarrierScratch) if (bb.Transition.pResource == res) { dup = true; break; }
        if (dup) continue;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = cur;
        b.Transition.StateAfter  = want;
        m_BarrierScratch.push_back(b);
        if (isBuffer) m_BufferStates[res] = want;
        else          m_Device->SetResourceState(res, want);
    }
    const UINT64 roDsv = (needReadOnlyDsv && !m_DsvReadOnly) ? m_Device->GetReadOnlyDsv(static_cast<UINT64>(m_PassDsv.ptr)) : 0;
    if (roDsv) {
        const D3D12_RESOURCE_STATES want = D3D12_RESOURCE_STATE_DEPTH_READ | kRead;
        const D3D12_RESOURCE_STATES cur  = m_Device->GetResourceState(m_PassDsvRes);
        if (cur != want) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource   = m_PassDsvRes;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = cur;
            b.Transition.StateAfter  = want;
            m_BarrierScratch.push_back(b);
            m_Device->SetResourceState(m_PassDsvRes, want);
        }
    }
    if (!m_BarrierScratch.empty())
        m_List->ResourceBarrier(static_cast<UINT>(m_BarrierScratch.size()), m_BarrierScratch.data());
    if (roDsv) {
        D3D12_CPU_DESCRIPTOR_HANDLE ro{};
        ro.ptr = static_cast<SIZE_T>(roDsv);
        m_List->OMSetRenderTargets(m_PassRtvCount, m_PassRtvCount ? m_PassRtvs : nullptr, FALSE, &ro);
        m_DsvReadOnly = true;
    }
}

void D3D12CommandList::SetRootTable(u32 param, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
    if (param >= 32u) return;
    if (m_RootTables[param].ptr == handle.ptr) return;   // 未变：不必重发（切管线另有标脏）
    m_RootTables[param] = handle;
    m_RootArgsDirty = true;
}

void D3D12CommandList::BufferBarrier(IRHIBuffer* buffer, ResourceState, ResourceState dstState) {
    if (!m_List || !buffer) return;
    TransitionBuffer(static_cast<D3D12Buffer*>(buffer)->GetResource(), ToD3D12ResourceState(dstState, false));
}

void D3D12CommandList::FlushRootArgs() {
    EmitDrawLabel();      // 逐绘制标签：插在本次绘制 / 派发的命令之前
    PrepareSetTables();   // 集内容变了要换快照：会更新 SRV/UAV 表并标脏根参数
    if (!m_List || !m_Pipeline || !m_RootArgsDirty) return;
    // 描述符表必须在 SetDescriptorHeaps 之后才能设置
    if (!m_HeapsSet && m_Device && m_Device->EnsureDescriptorHeaps()) {
        ID3D12DescriptorHeap* heaps[2] = { m_Device->GetCbvSrvUavHeap(), m_Device->GetSetSamplerHeap() };
        if (heaps[0] && heaps[1]) {
            m_List->SetDescriptorHeaps(2, heaps);
            m_HeapsSet = true;
        }
    }
    // 推送常量（root CBV b0/b1）：Begin 已为本次录制分配了槽位，
    // 即使本次绘制没有推送也要绑上，否则着色器读到未初始化的根参数会让 GPU 挂起
    if (m_PushGpu) {
        if (m_IsComputePipeline) {
            m_List->SetComputeRootConstantBufferView(0, m_PushGpu);
        } else {
            m_List->SetGraphicsRootConstantBufferView(0, m_PushGpu);
            m_List->SetGraphicsRootConstantBufferView(1, m_PushGpu);
        }
    }
    if (m_HeapsSet) {
        for (u32 i = 8; i < 32u; ++i) {
            if (!m_RootTables[i].ptr) continue;
            if (m_IsComputePipeline) m_List->SetComputeRootDescriptorTable(i, m_RootTables[i]);
            else                     m_List->SetGraphicsRootDescriptorTable(i, m_RootTables[i]);
        }
    }
    m_RootArgsDirty = false;
}

void D3D12CommandList::SetPushConstants(u32 offset, u32 size, const void* data) {
    if (!m_List || !m_Device || !data) return;

    // 【累积式推送】引擎一次"常量集"可能分多次推送：先 offset=0 推相机/帧常量，
    // 再用 offset>0 补逐物体数据。它们必须落在**同一块**常量缓冲里。
    // 只有当 offset==0（新的一组常量开始）时才另开槽位；其余情况继续写当前槽位。
    // 一次绘制的多次推送（相机 offset=0、逐物体 offset=84 等）必须共享同一槽位；
    // 只有「上一次绘制结束」后才另开槽位，否则后一次推送会覆盖掉前一次写入的字段
    //（实测：物体索引被相机推送覆盖成 0xCCCCCCCC ⇒ 所有绘制都画同一个物体）。
    if (m_PushDirty || m_PushGpu == 0 || !m_PushCpu) {
        D3D12_GPU_VIRTUAL_ADDRESS slot = 0;
        D3D12_GPU_DESCRIPTOR_HANDLE cbvHandle{};
        if (!m_Device->AllocateConstants(slot, cbvHandle)) return;
        void* cpu = m_Device->GetConstantSlotPtr(slot);
        if (!cpu) return;
        // 新槽位继承上一槽位的内容：Vulkan 的推送常量在被覆盖前一直有效，
        // 引擎会依赖这一点（例如只补推逐物体字段）；本次录制的第一个槽位从 0 开始
        if (m_PushCpu) std::memcpy(cpu, m_PushCpu, 256);
        else           std::memset(cpu, 0, 256);
        m_PushGpu = slot;
        m_PushCpu = cpu;
        m_PushDirty = false;
        if (m_IsComputePipeline) {
            m_List->SetComputeRootConstantBufferView(0, slot);
        } else {
            // 生成的 HLSL 里 FrameConstants 在 register(b0)/(b1)，两者都绑同一块
            m_List->SetGraphicsRootConstantBufferView(0, slot);
            m_List->SetGraphicsRootConstantBufferView(1, slot);
        }
    }

    // 记录本次推常量里的 objectIndex（偏移 84 = 第 21 个 u32），供绘制时核对
    if (offset <= 84u && offset + size >= 88u && data) {
        const u8* raw = static_cast<const u8*>(data);
        m_LastObjIdx = *reinterpret_cast<const u32*>(raw + (84u - offset));
    }
    if (offset >= 256u) return;
    const u32 copySize = ((offset + size) > 256u) ? (256u - offset) : size;
    std::memcpy(static_cast<unsigned char*>(m_PushCpu) + offset, data, copySize);
}

void D3D12CommandList::Draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) {
    // 诊断：每 60 次绘制打印一次（D3D12 的 GraphicsCommandList 没有 Get* 查询接口，
    // 所以这里打印我们自己记录的状态）
    if (TraceEnabled()) {
        static int s_draw = 0;
        if (++s_draw % 60 == 1) {
            std::printf("[DRAW] n=%d verts=%u inst=%u pso=%p rtvSet=%d vp=%.0fx%.0f\n",
                        s_draw, vertexCount, instanceCount,
                        static_cast<void*>(m_Pipeline),
                        m_HasRtv ? 1 : 0, m_LastVp.Width, m_LastVp.Height);
            std::fflush(stdout);
        }
    }
    if (!m_List) return;
    EnsureBoundResourceStates();
    FlushRootArgs();
    m_List->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
    AfterDrawOrDispatch();
    // 本次绘制已消费当前常量槽：之后的推送必须写新槽位，否则 GPU 执行时
    // 前面的绘制会读到后写入的值（实测天空盒读到后续全屏 pass 的矩阵，天空成纯色）
    m_PushDirty = true;
}

void D3D12CommandList::DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex,
                                   i32 vertexOffset, u32 firstInstance) {
    static const bool s_NoDraw = (std::getenv("HE_D3D12_NO_DRAW") != nullptr);   // 实验开关：跳过实际绘制（只读一次环境变量）
    if (s_NoDraw) return;
    if (!m_List) return;
    m_PushDirty = true;   // 下一次推送开始新的常量槽位
    EnsureBoundResourceStates();
    FlushRootArgs();
    m_List->DrawIndexedInstanced(indexCount, instanceCount, firstIndex,
                                 static_cast<INT>(vertexOffset), firstInstance);
    AfterDrawOrDispatch();
}

ID3D12CommandSignature* D3D12CommandList::GetCommandSignature(D3D12_INDIRECT_ARGUMENT_TYPE type, u32 stride) {
    if (!m_Device || !m_Device->GetDevice()) return nullptr;
    // 步长为 0 时取该参数类型的紧凑大小
    UINT want = stride;
    if (!want) {
        switch (type) {
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:         want = sizeof(D3D12_DRAW_ARGUMENTS);         break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: want = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS); break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:     want = sizeof(D3D12_DISPATCH_ARGUMENTS);     break;
            default: return nullptr;
        }
    }
    const u64 key = (static_cast<u64>(type) << 32) | want;
    // 命令签名必须活到 GPU 执行完，否则间接调用无效（id=921）：缓存到列表销毁为止
    if (auto it = m_CmdSigs.find(key); it != m_CmdSigs.end()) return it->second.Get();
    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = type;
    D3D12_COMMAND_SIGNATURE_DESC sd{};
    sd.ByteStride       = want;
    sd.NumArgumentDescs = 1;
    sd.pArgumentDescs   = &arg;
    sd.NodeMask         = 0;
    ComPtr<ID3D12CommandSignature> created;
    if (FAILED(m_Device->GetDevice()->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&created)))) {
        HE_CORE_ERROR("D3D12: 创建命令签名失败（类型 {}，步长 {}）", static_cast<int>(type), want);
        return nullptr;
    }
    return (m_CmdSigs[key] = created).Get();
}

void D3D12CommandList::DrawIndexedIndirect(IRHIBuffer* buffer, u64 offset, u32 drawCount, u32 stride) {
    if (!m_List || !buffer) return;
    auto* b = static_cast<D3D12Buffer*>(buffer);
    ID3D12CommandSignature* sig = GetCommandSignature(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED, stride);
    if (!sig) return;
    // 参数缓冲常由 compute（GPU 剔除）以 UAV 写出：执行前必须转到 INDIRECT_ARGUMENT
    TransitionBuffer(b->GetResource(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    EnsureBoundResourceStates();
    FlushRootArgs();
    m_List->ExecuteIndirect(sig, drawCount, b->GetResource(), offset, nullptr, 0);
    AfterDrawOrDispatch();
    m_PushDirty = true;   // 同 Draw：常量槽已被本次绘制消费
}

void D3D12CommandList::DrawIndexedIndirectCount(IRHIBuffer* buffer, u64 offset,
                                                IRHIBuffer* countBuffer, u64 countOffset,
                                                u32 maxDrawCount, u32 stride) {
    if (!m_List || !buffer || !countBuffer) return;
    auto* b  = static_cast<D3D12Buffer*>(buffer);
    auto* cb = static_cast<D3D12Buffer*>(countBuffer);
    // 签名走缓存：此前每次调用都建局部 ComPtr，函数返回即销毁而 GPU 尚未执行（同 id=921）
    ID3D12CommandSignature* sig = GetCommandSignature(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED, stride);
    if (!sig) return;
    // 参数缓冲与计数缓冲都要处于 INDIRECT_ARGUMENT（同一缓冲兼作两者时只转一次）
    TransitionBuffer(b->GetResource(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    TransitionBuffer(cb->GetResource(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    // D3D12 的 ExecuteIndirect 原生支持「计数缓冲」，等价 Vulkan 的 *IndirectCount
    EnsureBoundResourceStates();
    FlushRootArgs();
    m_List->ExecuteIndirect(sig, maxDrawCount, b->GetResource(), offset,
                            cb->GetResource(), countOffset);
    AfterDrawOrDispatch();
    m_PushDirty = true;   // 同 Draw：常量槽已被本次绘制消费
}

void D3D12CommandList::Dispatch(u32 x, u32 y, u32 z) {
    if (!m_List) return;
    EnsureBoundResourceStates();
    FlushRootArgs();
    m_List->Dispatch(x, y, z);
    AfterDrawOrDispatch();
    m_PushDirty = true;   // 同 Draw：常量槽已被本次派发消费
}

void D3D12CommandList::DispatchIndirect(IRHIBuffer* buffer, u64 offset) {
    if (!m_List || !buffer) return;
    auto* b = static_cast<D3D12Buffer*>(buffer);
    // 此前签名传空指针：ExecuteIndirect 要求有效的命令签名（派发参数 = 3 个 u32）
    ID3D12CommandSignature* sig = GetCommandSignature(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH, 0);
    if (!sig) return;
    TransitionBuffer(b->GetResource(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    EnsureBoundResourceStates();
    FlushRootArgs();
    m_List->ExecuteIndirect(sig, 1, b->GetResource(), offset, nullptr, 0);
    AfterDrawOrDispatch();
    m_PushDirty = true;   // 同 Draw：常量槽已被本次派发消费
}

void D3D12CommandList::PipelineBarrier(PipelineStage, PipelineStage,
                                       ResourceState, ResourceState) {
    // 无资源参数的全局屏障：D3D12 要求屏障绑定到具体资源，
    // 引擎里这种调用通常用于「pass 之间的全局同步」，D3D12 下由 UAV 屏障近似。
    if (!m_List) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.UAV.pResource = nullptr;   // nullptr = 全局 UAV 屏障（所有 UAV）
    m_List->ResourceBarrier(1, &b);
}

void D3D12CommandList::PipelineBarrier(PipelineStage, PipelineStage,
                                       ResourceState srcState, ResourceState dstState,
                                       IRHITexture* texture) {
    if (!m_List || !texture) return;
    auto* t = static_cast<D3D12Texture*>(texture);
    const D3D12_RESOURCE_STATES from = ToD3D12ResourceState(srcState, t->IsDepth());
    const D3D12_RESOURCE_STATES to   = ToD3D12ResourceState(dstState, t->IsDepth());
    // 【UAV → UAV 必须是 UAV 屏障】引擎在相互依赖的计算派发之间发「UAV → UAV」屏障（如 SDF 的清空 → 散射 →
    // 逐轮洪泛），Vulkan 下它是内存屏障。此前状态相同就直接返回、什么都不做：前后派发在 GPU 上可以重叠执行，
    // 后一次读到前一次未写完的数据（实测 Lumen 逐 mesh 距离场在 D3D12 下大片未写入 / 误差偏大）
    auto uavBarrier = [&]() {
        D3D12_RESOURCE_BARRIER ub{};
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = t->GetResource();
        m_List->ResourceBarrier(1, &ub);
    };
    if (from == to) {
        if (to == D3D12_RESOURCE_STATE_UNORDERED_ACCESS &&
            (!m_Device || m_Device->GetResourceState(t->GetResource()) == to)) uavBarrier();
        return;
    }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource   = t->GetResource();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        // 【用我们登记的真实状态做屏障】引擎传的 srcState 未必等于资源的实际状态（它与引擎自己的跟踪有关，D3D12 侧未必一致），直接照用会让 ResourceBarrier 被判非法（调试层 id=527，实测 3600 条），屏障无效。 这里以登记状态为准。
        ID3D12Resource* bres = t->GetResource();
        const D3D12_RESOURCE_STATES actualFrom =
            m_Device ? m_Device->GetResourceState(bres) : from;
        if (actualFrom == to) {
            if (to == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) uavBarrier();   // 已是 UAV：仍要同步前后派发
            return;
        }
        b.Transition.StateBefore = actualFrom;
    b.Transition.StateAfter  = to;
    m_List->ResourceBarrier(1, &b);
        if (m_Device) m_Device->SetResourceState(bres, to);
}

void D3D12CommandList::CopyBuffer(IRHIBuffer* src, IRHIBuffer* dst, u64 size,
                                  u64 srcOffset, u64 dstOffset) {
    if (!m_List || !src || !dst) return;
    auto* s = static_cast<D3D12Buffer*>(src);
    auto* d = static_cast<D3D12Buffer*>(dst);
    // 拷贝要求源 / 目标分别处于 COPY_SOURCE / COPY_DEST：同一列表里缓冲可能已被提升为 UAV 等状态，
    // 这时必须显式转换（只处理 DEFAULT/CUSTOM 堆；UPLOAD/READBACK 堆的缓冲状态固定）
    TransitionBuffer(s->GetResource(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionBuffer(d->GetResource(), D3D12_RESOURCE_STATE_COPY_DEST);
    m_List->CopyBufferRegion(d->GetResource(), dstOffset, s->GetResource(), srcOffset, size);
}

void D3D12CommandList::TransitionBuffer(ID3D12Resource* res, D3D12_RESOURCE_STATES want) {
    if (!res || !m_Device) return;
    D3D12_HEAP_PROPERTIES hp{};
    if (FAILED(res->GetHeapProperties(&hp, nullptr)) ||
        (hp.Type != D3D12_HEAP_TYPE_DEFAULT && hp.Type != D3D12_HEAP_TYPE_CUSTOM)) return;
    const D3D12_RESOURCE_STATES cur = GetBufferState(res);
    if (cur == want) return;
    // 本列表内首次使用：由 COMMON 隐式提升，不发屏障（见 EnsureBoundResourceStates）
    if (cur == D3D12_RESOURCE_STATE_COMMON) { m_BufferStates[res] = want; return; }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = cur;
    b.Transition.StateAfter  = want;
    m_List->ResourceBarrier(1, &b);
    m_BufferStates[res] = want;   // 本列表内的缓冲状态（列表执行完后衰减回 COMMON，见 Begin）
}

void D3D12CommandList::CopyTextureToTexture(IRHITexture* src, IRHITexture* dst) {
    if (!m_List || !src || !dst) return;
    auto* s = static_cast<D3D12Texture*>(src);
    auto* d = static_cast<D3D12Texture*>(dst);
    ID3D12Resource* sr = s->GetResource();
    ID3D12Resource* dr = d->GetResource();
    if (!sr || !dr || sr == dr || !m_Device) return;
    // CopyResource 要求源 / 目标分别处于 COPY_SOURCE / COPY_DEST：临时转过去，拷完转回原状态
    //（设备级状态记录不变）。此前不做转换，渲染目标等状态下直接拷贝属非法调用
    const D3D12_RESOURCE_STATES ss = m_Device->GetResourceState(sr);
    const D3D12_RESOURCE_STATES ds = m_Device->GetResourceState(dr);
    D3D12_RESOURCE_BARRIER b[2]{};
    UINT nb = 0;
    auto add = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        if (from == to) return;
        b[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[nb].Transition.pResource   = r;
        b[nb].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[nb].Transition.StateBefore = from;
        b[nb].Transition.StateAfter  = to;
        ++nb;
    };
    add(sr, ss, D3D12_RESOURCE_STATE_COPY_SOURCE);
    add(dr, ds, D3D12_RESOURCE_STATE_COPY_DEST);
    if (nb) m_List->ResourceBarrier(nb, b);
    m_List->CopyResource(dr, sr);
    nb = 0;
    add(sr, D3D12_RESOURCE_STATE_COPY_SOURCE, ss);
    add(dr, D3D12_RESOURCE_STATE_COPY_DEST, ds);
    if (nb) m_List->ResourceBarrier(nb, b);
}

void D3D12CommandList::CopyTextureToBuffer(IRHITexture* src, IRHIBuffer* dst, u32 x, u32 y,
                                           u32 width, u32 height, u64 bufferOffset) {
    if (!m_List || !m_Device || !m_Device->GetDevice() || !src || !dst) return;
    auto* t = static_cast<D3D12Texture*>(src);
    auto* b = static_cast<D3D12Buffer*>(dst);
    ID3D12Resource* texRes = t->GetResource();
    ID3D12Resource* bufRes = b->GetResource();
    if (!texRes || !bufRes) return;
    const u32 w = width  ? width  : t->GetWidth();
    const u32 h = height ? height : t->GetHeight();
    if (w == 0 || h == 0 || x + w > t->GetWidth() || y + h > t->GetHeight()) return;
    D3D12_RESOURCE_DESC rd = texRes->GetDesc();
    if (rd.SampleDesc.Count > 1) return;   // 多重采样纹理不能直接拷贝

    // 【两步拷贝】D3D12 的纹理 → 缓冲拷贝要求行距按 256 字节、起点按 512 字节对齐，
    // 而引擎期望从任意 bufferOffset 起紧密排列（像素探针把多个 1×1 像素按 8 字节间隔写进同一缓冲）。
    // 先拷到行距对齐的中转缓冲，再逐行用 CopyBufferRegion（无对齐要求）拷到目标位置。
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT   rows = 0;
    UINT64 rowBytes = 0, total = 0;
    m_Device->GetDevice()->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    if (rows == 0 || rowBytes == 0 || total == 0) return;
    if (bufferOffset + rowBytes * rows > b->GetSize()) {
        HE_CORE_ERROR("D3D12 CopyTextureToBuffer: 目标缓冲太小（需要 {} 字节，缓冲 {} 字节）",
                      bufferOffset + rowBytes * rows, static_cast<u64>(b->GetSize()));
        return;
    }

    // 中转缓冲：DEFAULT 堆，挂在本槽位的临时资源表上，GPU 执行完后（下次复用本槽位时）释放
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = total;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.Format           = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc       = { 1, 0 };
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> staging;
    if (FAILED(m_Device->GetDevice()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&staging)))) {
        HE_CORE_ERROR("D3D12 CopyTextureToBuffer: 中转缓冲创建失败（{} 字节）", total);
        return;
    }
    m_TempResources[m_FrameSlot].push_back(staging);

    // 纹理临时转到 COPY_SOURCE，拷完转回原状态（设备级记录不变）
    const D3D12_RESOURCE_STATES texState = m_Device->GetResourceState(texRes);
    D3D12_RESOURCE_BARRIER tb{};
    tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    tb.Transition.pResource   = texRes;
    tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    tb.Transition.StateBefore = texState;
    tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (texState != D3D12_RESOURCE_STATE_COPY_SOURCE) m_List->ResourceBarrier(1, &tb);

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource       = staging.Get();
    dstLoc.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource        = texRes;
    srcLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;
    const D3D12_BOX box{ x, y, 0, x + w, y + h, 1 };
    m_List->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &box);

    if (texState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        std::swap(tb.Transition.StateBefore, tb.Transition.StateAfter);
        m_List->ResourceBarrier(1, &tb);
    }

    // 中转缓冲 → COPY_SOURCE，目标缓冲 → COPY_DEST，然后逐行紧密拷贝（行距相同时一次拷完）
    D3D12_RESOURCE_BARRIER sb{};
    sb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    sb.Transition.pResource   = staging.Get();
    sb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    sb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    sb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    m_List->ResourceBarrier(1, &sb);
    TransitionBuffer(bufRes, D3D12_RESOURCE_STATE_COPY_DEST);
    if (fp.Footprint.RowPitch == rowBytes) {
        m_List->CopyBufferRegion(bufRes, bufferOffset, staging.Get(), 0, rowBytes * rows);
    } else {
        for (UINT r = 0; r < rows; ++r) {
            m_List->CopyBufferRegion(bufRes, bufferOffset + rowBytes * r,
                                     staging.Get(), static_cast<UINT64>(fp.Footprint.RowPitch) * r, rowBytes);
        }
    }
}

// 【时间线信号 / 等待附在提交上】Vulkan 把它们放进本次 vkQueueSubmit（命令执行完才 signal）。
// 此前这里在**录制时**就直接对队列 Signal：本列表尚未提交，信号排在之前的帧之后，很快到达，
// 等它的一方（帧转储读回等）以为拷贝已完成 ⇒ 读到全 0。改为记下来，由 Device::Submit 在
// ExecuteCommandLists 前后执行。
void D3D12CommandList::SetTimelineSignal(RHIFenceHandle fence, u64 value) {
    m_PendingSignals.emplace_back(fence, value);
}

void D3D12CommandList::SetTimelineWait(RHIFenceHandle fence, u64 value) {
    m_PendingWaits.emplace_back(fence, value);
}

void D3D12CommandList::ApplyTimelineWaits() {
    if (m_Device) for (const auto& [f, v] : m_PendingWaits) m_Device->WaitFenceOnQueue(QueueType::Graphics, f, v);
    m_PendingWaits.clear();
}

void D3D12CommandList::ApplyTimelineSignals() {
    if (m_Device) for (const auto& [f, v] : m_PendingSignals) m_Device->SignalFenceOnQueue(QueueType::Graphics, f, v);
    m_PendingSignals.clear();
}

void D3D12CommandList::Submit() {
    if (TraceEnabled()) { std::printf("[TRACE] Submit\n"); std::fflush(stdout); }
    if (m_Device) m_Device->Submit(this, nullptr);
}

} // namespace he::rhi::d3d12

#endif // _WIN32
