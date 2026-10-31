// ============================================================================
//  D3D12CommandList.cpp —— D3D12 命令录制的实现
// ============================================================================
#include "D3D12CommandList.h"
#include "D3D12Device.h"
#include "D3D12Resources.h"
#include "D3D12RayTracing.h"   // DXR：加速结构 / RT 状态对象 / 几何描述转换
#include "D3D12SwapChain.h"
#include "D3D12Converters.h"
#include "Core/CVar.h"   // r.Debug.DrawMarker（与 Vulkan 后端共用的逐绘制标签开关）
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>

#if defined(_WIN32)

namespace he::rhi::d3d12 {

// ---------------------------------------------------------------------------
// 【P3】命令流是否承载光追（默认开启；HE_RHI_COMMAND_STREAM_RT=0 可回退）
//
// 背景：T10 的设计是「命中即回退」——BLAS / TLAS 构建、RT 管线绑定、DispatchRays 一律
// 标记为无法承载，从下一帧起帧主列表改回直接录制（画面自下一帧恢复，绝不静默丢命令）。
// P3 把这四类命令都编码进了命令流（`CmdBuildBLAS` / `CmdBuildTLAS` / `CmdBindRTPipeline` /
// `CmdTraceRays`，几何与 SBT 槽位随流 arena 走），回放侧重建描述结构后下发原生调用。
//
// 【曾经的现象与根因】启用流式光追后 05 截图退化为 16 KB 纯天空（直录为 978 KB），且
// 0 条 D3D12 错误、0 次设备丢失。根因**不在**描述符快照或 AS 生命周期，而在回放侧取
// `CmdBuildBLAS` 载荷时用错了接口：录制侧用 `EnqueueArena` 编码（内联 payloadSize 为 0、
// 数据在 arena），回放侧却用 `PayloadAt`（只读内联载荷）取，长度校验必然失败返回空指针，
// 于是**所有 BLAS 构建命令被静默跳过**；TLAS 实例指向从未构建过的 BLAS ⇒ 光线全部未命中
// ⇒ 只剩天空。修复见 RHICommandReplay.cpp（改用 `ArenaPayloadAt`，并对坏载荷留日志）。
//
// 现在默认由命令流承载光追；设 HE_RHI_COMMAND_STREAM_RT=0 则整帧直接录制（命令流不接管光追）。
// 诊断：HE_D3D12_STREAM_RT_DBG=1 会打印录制/回放两侧的描述符表槽位、AS 地址与派发根参数。
// ---------------------------------------------------------------------------
static bool StreamRTCarryEnabled() {
    static const bool s_on = [] {
        const char* v = std::getenv("HE_RHI_COMMAND_STREAM_RT");
        // 默认开启；只有显式 =0（或以 0 开头）才回退
        return (v == nullptr) || (v[0] != '0');
    }();
    return s_on;
}

// ---------------------------------------------------------------------------
//  【P3 诊断】HE_D3D12_STREAM_RT_DBG=1 时，把描述符 GPU 句柄换算成「堆内槽位」再打印。
//  直接录制与命令流回放两条路径都用同一个换算，便于逐位对比两者绑定的表到底是哪一块。
// ---------------------------------------------------------------------------
static bool StreamRTDebugEnabled() {
    static const bool s_on = (std::getenv("HE_D3D12_STREAM_RT_DBG") != nullptr);
    return s_on;
}

static u32 DbgDescSlot(D3D12Device* dev, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    if (!dev || h.ptr == 0) return 0xFFFFFFFFu;
    const D3D12_GPU_DESCRIPTOR_HANDLE base = dev->GetSrvHeapGpuStart();
    const UINT inc = dev->GetCbvSrvUavSize();
    if (inc == 0 || h.ptr < base.ptr) return 0xFFFFFFFFu;
    return static_cast<u32>((h.ptr - base.ptr) / inc);
}

D3D12CommandList::~D3D12CommandList() {
    if (!m_Device) return;
    m_Device->UnregisterCommandList(this);
    // 【GPU 可能仍在执行】RenderGraph 每帧新建异步计算列表，提交后函数返回即析构：此前原生列表、
    // 分配器、命令签名、临时资源随之立即释放，GPU 还在读它们（命令内存被回收 ⇒ 页错误 / 设备移除）。
    // 取各槽位最后一次提交的围栏值，未完成就交给设备延迟到围栏到达后再释放
    std::vector<ComPtr<IUnknown>> keep;
    if (m_List) keep.emplace_back(m_List);
    for (auto& l : m_Lists) if (l) keep.emplace_back(l);
    for (auto& a : m_Allocators) if (a) keep.emplace_back(a);
    for (auto& [key, sig] : m_CmdSigs) if (sig) keep.emplace_back(sig);
    for (auto& slot : m_TempResources) for (auto& r : slot) if (r) keep.emplace_back(r);
    if (m_IsBundle) {
        // bundle 自己不提交：按执行过它的主列表的提交围栏延迟释放（未完成的每一次都挂上）
        for (const auto& u : m_BundleSlotUses)
            if (u.fence && u.fence->GetCompletedValue() < u.value) m_Device->DeferRelease(u.fence, u.value, keep);
        return;
    }
    u64 last = 0;
    for (u64 v : m_SlotFenceValues) last = std::max(last, v);
    if (!m_SlotFence || last == 0 || m_SlotFence->GetCompletedValue() >= last) return;
    m_Device->DeferRelease(m_SlotFence, last, std::move(keep));
}

bool D3D12CommandList::Initialize(D3D12Device* device, QueueType queue) {
    if (!device || !device->GetDevice()) return false;
    m_Device = device;
    ID3D12Device* dev = device->GetDevice();
    // 没有独立计算队列时计算列表按图形列表创建（COMPUTE 列表不能提交到图形队列）
    m_QueueType = (queue == QueueType::Compute && device->HasAsyncComputeQueue()) ? QueueType::Compute
                                                                                  : QueueType::Graphics;

    // 图形的 DIRECT 队列 / 计算的 COMPUTE 队列
    const D3D12_COMMAND_LIST_TYPE type =
        (m_QueueType == QueueType::Compute) ? D3D12_COMMAND_LIST_TYPE_COMPUTE
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
    // 每个帧槽位一个原生列表（见 m_Lists 的说明）
    for (u32 i = 0; i < kMaxFramesInFlight; ++i) {
        if (!HE_D3D12_CHECK(dev->CreateCommandList(0, type, m_Allocators[i].Get(), nullptr,
                                                   IID_PPV_ARGS(&m_Lists[i])),
                            "CreateCommandList")) {
            return false;
        }
        // 命名：DRED 报告与 RenderDoc 里据此区分图形 / 计算列表
        m_Lists[i]->SetName(m_QueueType == QueueType::Compute ? L"HugEngine 计算命令列表" : L"HugEngine 图形命令列表");
        // 创建后默认是开着的，先关掉，等 Begin() 再 Reset
        m_Lists[i]->Close();
    }
    m_List = m_Lists[0];
    m_List.As(&m_List6);   // 网格着色器绘制用（取不到时 DrawMeshTasks* 忽略）
    m_Device->RegisterCommandList(this);   // DRED 报告据此把面包屑对应回事件名
    return true;
}

bool D3D12CommandList::InitializeBundle(D3D12Device* device) {
    if (!device || !device->GetDevice()) return false;
    m_Device   = device;
    m_IsBundle = true;
    ID3D12Device* dev = device->GetDevice();
    for (auto& alloc : m_Allocators) {
        if (!HE_D3D12_CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&alloc)),
                            "CreateCommandAllocator(Bundle)")) {
            return false;
        }
    }
    m_Allocator = m_Allocators[0];
    if (!HE_D3D12_CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, m_Allocator.Get(), nullptr,
                                               IID_PPV_ARGS(&m_List)),
                        "CreateCommandList(Bundle)")) {
        return false;
    }
    m_List.As(&m_List6);   // bundle 里同样可以 DispatchMesh
    m_Device->RegisterCommandList(this);
    m_List->SetName(L"HugEngine 二级命令列表（bundle）");
    m_List->Close();
    return true;
}

void D3D12CommandList::BeginSecondary(IRHIPipelineState* pso) {
    if (!m_IsBundle || !m_List) return;
    // 轮换槽位；该槽位上一次被执行的主列表提交在 GPU 上完成之前，分配器不能 Reset
    m_FrameSlot = (m_FrameSlot + 1) % kMaxFramesInFlight;
    m_Allocator = m_Allocators[m_FrameSlot];
    auto& use = m_BundleSlotUses[m_FrameSlot];
    if (use.fence && use.fence->GetCompletedValue() < use.value) {
        if (HANDLE ev = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS)) {
            use.fence->SetEventOnCompletion(use.value, ev);
            WaitForSingleObject(ev, 8000);   // 有上限：设备挂起时不至于永久卡住
            CloseHandle(ev);
        }
    }
    m_TempResources[m_FrameSlot].clear();
    auto* initPso = static_cast<D3D12PipelineState*>(pso);
    if (FAILED(m_Allocator->Reset()) ||
        FAILED(m_List->Reset(m_Allocator.Get(), initPso ? initPso->GetPso() : nullptr))) {
        HE_CORE_ERROR("D3D12CommandList::BeginSecondary: bundle Reset 失败");
        return;
    }
    m_Recording = true;
    ResetRecordingState();
    m_BundleUsages.clear();
    m_BundleBufferTransitions.clear();
    if (pso) SetPipeline(pso);
}

void D3D12CommandList::ExecuteSecondary(IRHICommandList* secondary) {
    auto* b = static_cast<D3D12CommandList*>(secondary);
    if (m_StreamMode) {
        // 【T10】流模式：bundle 的状态转换必须在录制期解析（回放侧不再查描述符集 / 设备状态表），
        // 然后把"执行 bundle"这一条命令入流；bundle 自身的原生列表仍由录制者线程真实录制。
        if (b && b->m_IsBundle && m_Device) {
            D3D12PipelineState* savedPso = m_Pipeline;
            const bool savedCompute = m_IsComputePipeline;
            DescriptorSetHandle savedSets[8];
            std::copy(std::begin(m_BoundSets), std::end(m_BoundSets), savedSets);
            for (const auto& u : b->m_BundleUsages) {
                m_Pipeline          = u.pipeline;
                m_IsComputePipeline = u.compute;
                std::copy(std::begin(u.sets), std::end(u.sets), std::begin(m_BoundSets));
                EnsureBoundResourceStates();
            }
            m_Pipeline          = savedPso;
            m_IsComputePipeline = savedCompute;
            std::copy(std::begin(savedSets), std::end(savedSets), std::begin(m_BoundSets));
            for (const auto& [res, want] : b->m_BundleBufferTransitions) TransitionBuffer(res, want);
            AfterDrawOrDispatch();
            m_SetRingChunkMask |= b->m_SetRingChunkMask;   // bundle 用过的快照块随本帧提交登记围栏
        }
        // 【T10】与原生路径的第 4 步一致：bundle 改过的管线 / 根参数 / IA 状态会延续到主列表，
        // 录制侧缓存必须作废 —— 否则下一次绘制不会重新拍描述符表（回放侧的缓存已被清除，
        // 但表数据只随"重新拍快照"入流，两边会不一致）。
        for (auto& t : m_RootTables) t.ptr = 0;
        for (auto& ss : m_SnapSets) ss = kInvalidSet;
        m_RootArgsDirty = true;
        m_PushDirty     = true;
        if (m_Pipeline) SetPipeline(m_Pipeline);
        Stream().Enqueue(RHICommandType::ExecuteSecondary, RHICommandExecuteSecondary{ secondary });
        return;
    }
    if (!m_List || !m_Recording || !m_Device || !b || !b->m_IsBundle || !b->m_List) return;
    // 1. 资源状态：bundle 里不能发屏障，按它记下的每次绘制的管线 + 绑定集，在主列表上补状态转换
    //    【T10】回放侧跳过：这些转换在录制期已编码进命令流
    if (!m_StreamReplay) {
        D3D12PipelineState* savedPso = m_Pipeline;
        const bool savedCompute = m_IsComputePipeline;
        DescriptorSetHandle savedSets[8];
        std::copy(std::begin(m_BoundSets), std::end(m_BoundSets), savedSets);
        for (const auto& u : b->m_BundleUsages) {
            m_Pipeline          = u.pipeline;
            m_IsComputePipeline = u.compute;
            std::copy(std::begin(u.sets), std::end(u.sets), std::begin(m_BoundSets));
            EnsureBoundResourceStates();
        }
        m_Pipeline          = savedPso;
        m_IsComputePipeline = savedCompute;
        std::copy(std::begin(savedSets), std::end(savedSets), std::begin(m_BoundSets));
        for (const auto& [res, want] : b->m_BundleBufferTransitions) TransitionBuffer(res, want);
        AfterDrawOrDispatch();   // 按子资源拆分的转换在这里没有绘制可依附：立即转回统一状态
    }

    // 2. bundle 用到描述符表时，主列表必须先绑定同一组描述符堆
    if (!m_HeapsSet && m_Device->EnsureDescriptorHeaps()) {
        ID3D12DescriptorHeap* heaps[2] = { m_Device->GetCbvSrvUavHeap(), m_Device->GetSetSamplerHeap() };
        if (heaps[0] && heaps[1]) { m_List->SetDescriptorHeaps(2, heaps); m_HeapsSet = true; }
    }

    // 3. 执行，并记下：bundle 用过的描述符快照块、提交时要给 bundle 槽位打的围栏
    m_List->ExecuteBundle(b->m_List.Get());
    if (!m_StreamReplay) m_SetRingChunkMask |= b->m_SetRingChunkMask;   // 回放：录制期已并入流提交包
    m_ExecutedBundles.emplace_back(b, b->m_FrameSlot);
    m_TotalDraws += b->m_TotalDraws;

    // 4. bundle 改过的管线、根签名、根参数、IA 状态会延续到主列表：主列表缓存全部作废，下次绘制重新下发
    for (auto& t : m_RootTables) t.ptr = 0;
    for (auto& ss : m_SnapSets) ss = kInvalidSet;
    m_RootArgsDirty = true;
    m_PushDirty     = true;
    if (m_Pipeline) SetPipeline(m_Pipeline);
}

void D3D12CommandList::Begin() {
    if (m_StreamMode) {
        // 流模式：不碰原生列表与分配器（由影子列表负责），只轮换到下一份命令流并清空
        m_StreamSlot = (m_StreamSlot + 1) % kStreamSlots;
        if (!m_Streams[m_StreamSlot]) m_Streams[m_StreamSlot] = std::make_unique<RHICommandList>();
        m_Streams[m_StreamSlot]->Reset();
        // 【T10】录制期解析用的缓存必须按帧作废：描述符快照的"集 + 版本"、绑定集、缓冲状态、
        // 快照环块掩码全都只对本次录制成对，沿用上一帧会让新帧误以为快照仍然有效（内容却是旧块的）。
        // 流模式不写原生列表，推送常量走命令流 arena ⇒ 不占常量环槽位。
        ResetRecordingState(false);
        m_Recording = true;
        return;
    }
    if (!m_List || !m_Allocator) return;
    // 轮换到下一个帧槽位，并等待该槽位上一次提交的命令在 GPU 上执行完，再复用其分配器与原生列表
    m_FrameSlot = (m_FrameSlot + 1) % kMaxFramesInFlight;
    m_Allocator = m_Allocators[m_FrameSlot];
    if (m_Lists[m_FrameSlot]) {
        m_List = m_Lists[m_FrameSlot];
        m_List6.Reset();
        m_List.As(&m_List6);
        m_List4.Reset();          // DXR 接口随列表轮换，懒取（GetList4）
    }
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
    ResetRecordingState();
}

void D3D12CommandList::ResetRecordingState(bool allocatePush) {
    // 新的录制：列表的根参数与描述符堆都已失效，清空缓存
    for (auto& t : m_RootTables) t.ptr = 0;
    m_RootArgsDirty = true;
    m_HeapsSet = false;
    m_RootSigBound = false;   // 新录制：尚未绑定任何根签名（未绑时不许下发根参数，见头文件说明）
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
    m_ExecutedBundles.clear();  // 上一次录制执行过的 bundle 已在提交时处理
    m_QueueHandoffs.clear();    // 上一次录制若未提交，残留的交接转换作废
    ClearPassAttachments();
    // 缓冲在每次提交执行完后衰减回 COMMON：新录制从全 COMMON 开始
    m_BufferStates.clear();
    // 为整帧申请一块推送常量槽位，并清零
    m_PushGpu = 0;
    m_PushCpu = nullptr;
    m_PushDirty = true;
    if (!allocatePush) return;   // 命令流录制：推送常量走流 arena，不需要常量环槽位
    D3D12_GPU_DESCRIPTOR_HANDLE dummyCbv{};
    if (m_Device && m_Device->AllocateConstants(m_PushGpu, dummyCbv)) {
        m_PushCpu = m_Device->GetConstantSlotPtr(m_PushGpu);
        if (m_PushCpu) std::memset(m_PushCpu, 0, 256);
    }
}

void D3D12CommandList::PackSubmission() {
    SubmitPacket p;
    p.native           = m_List.Get();
    p.slot             = m_FrameSlot;
    p.waits            = std::move(m_PendingWaits);
    p.signals          = std::move(m_PendingSignals);
    p.handoffs         = std::move(m_QueueHandoffs);
    p.queries          = std::move(m_PendingQueries);
    p.bundles          = std::move(m_ExecutedBundles);
    p.setRingChunkMask = m_SetRingChunkMask;
    m_PendingWaits.clear();
    m_PendingSignals.clear();
    m_QueueHandoffs.clear();
    m_PendingQueries.clear();
    m_ExecutedBundles.clear();
    m_SetRingChunkMask = 0;
    std::lock_guard<std::mutex> lock(m_SubmitPacketMutex);
    m_SubmitPackets.push_back(std::move(p));
}

bool D3D12CommandList::TakeSubmission(SubmitPacket& out) {
    std::lock_guard<std::mutex> lock(m_SubmitPacketMutex);
    if (m_SubmitPackets.empty()) return false;
    out = std::move(m_SubmitPackets.front());
    m_SubmitPackets.pop_front();
    return true;
}

void D3D12CommandList::OnSubmitted(ID3D12CommandQueue* queue, const SubmitPacket& packet) {
    if (!queue || !m_SlotFence) return;
    const u64 v = m_NextFenceValue++;
    queue->Signal(m_SlotFence.Get(), v);
    m_SlotFenceValues[packet.slot] = v;
    // 本次录制用过的描述符集快照块：围栏到达 v 前不能被覆盖
    if (m_Device && packet.setRingChunkMask) m_Device->MarkSetRingChunks(packet.setRingChunkMask, m_SlotFence.Get(), v);
    // 本次提交写过的查询：围栏到达 v 后结果可读
    for (const auto& [pool, idx] : packet.queries) pool->MarkSubmitted(idx, m_SlotFence.Get(), v);
    // 本次提交执行过的 bundle：其该槽位的分配器要等围栏到达 v 才能复用
    for (const auto& [bundle, slot] : packet.bundles) bundle->m_BundleSlotUses[slot] = { m_SlotFence, v };
}

// ── 调试标签 ──
// PIX 事件的 Metadata：0 = UTF-16 字符串（WINPIX_EVENT_UNICODE_VERSION），数据含结尾的 0。
// 不链接 WinPixEventRuntime，直接用 ID3D12GraphicsCommandList 的 BeginEvent / SetMarker。
namespace {
constexpr UINT kPixEventUnicode = 0;
}

void D3D12CommandList::BeginDebugLabel(const char* name, const float*) {
    if (m_StreamMode) { StreamLabel(RHICommandType::CmdBeginDebugLabel, name); return; }
    if (!m_List || !m_Recording) return;
    const std::wstring w = Utf8ToWide(name ? name : "");
    m_List->BeginEvent(kPixEventUnicode, w.c_str(), static_cast<UINT>((w.size() + 1) * sizeof(wchar_t)));
    ++m_LabelDepth;
    if (DredEventLogEnabled()) m_EventLogs[m_FrameSlot].begins.emplace_back(name ? name : "");
}

void D3D12CommandList::EndDebugLabel() {
    if (m_StreamMode) { Stream().Enqueue(RHICommandType::CmdEndDebugLabel, u32{0}); return; }
    if (!m_List || !m_Recording || m_LabelDepth == 0) return;   // 不配对的 End 直接忽略（D3D12 会判非法）
    m_List->EndEvent();
    --m_LabelDepth;
}

void D3D12CommandList::InsertDebugLabel(const char* name, const float*) {
    if (m_StreamMode) { StreamLabel(RHICommandType::CmdInsertDebugLabel, name); return; }
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
    bool mine = native && native == static_cast<const ID3D12CommandList*>(m_List.Get());
    for (const auto& l : m_Lists) mine = mine || (native && native == static_cast<const ID3D12CommandList*>(l.Get()));
    if (!mine) return false;
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
    if (m_StreamMode) { StreamLabel(RHICommandType::SetDrawDebugLabel, name); return; }
    // 空标签视为清除，避免残留到下一个绘制
    if (!name || !*name) { m_DrawLabelSet = false; return; }
    m_DrawLabel    = name;
    m_DrawLabelSet = true;
}

void D3D12CommandList::ClearDrawDebugLabel() {
    if (m_StreamMode) { Stream().Enqueue(RHICommandType::CmdClearDrawDebugLabel, u32{0}); return; }
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
    if (m_StreamMode) { Stream().Enqueue(RHICommandType::CmdWriteTimestamp, RHICommandQuery{ pool, queryIndex, 0u }); return; }
    auto* qp = static_cast<D3D12QueryPool*>(pool);
    if (!m_List || !qp || queryIndex >= qp->GetQueryCount() || qp->GetQueryType() != QueryType::Timestamp) return;
    m_List->EndQuery(qp->GetHeap(), D3D12_QUERY_TYPE_TIMESTAMP, queryIndex);
    // 写完立刻解析到回读缓冲（READBACK 堆一直处于 COPY_DEST，无需屏障）
    m_List->ResolveQueryData(qp->GetHeap(), D3D12_QUERY_TYPE_TIMESTAMP, queryIndex, 1,
                             qp->GetReadback(), static_cast<UINT64>(queryIndex) * qp->GetResultStride());
    m_PendingQueries.emplace_back(qp, queryIndex);
}

void D3D12CommandList::BeginQuery(IRHIQueryPool* pool, u32 queryIndex) {
    if (m_StreamMode) { Stream().Enqueue(RHICommandType::CmdBeginQuery, RHICommandQuery{ pool, queryIndex, 0u }); return; }
    auto* qp = static_cast<D3D12QueryPool*>(pool);
    if (!m_List || !qp || queryIndex >= qp->GetQueryCount() ||
        qp->GetQueryType() != QueryType::PipelineStatistics) return;
    m_List->BeginQuery(qp->GetHeap(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, queryIndex);
}

void D3D12CommandList::EndQuery(IRHIQueryPool* pool, u32 queryIndex) {
    if (m_StreamMode) { Stream().Enqueue(RHICommandType::CmdEndQuery, RHICommandQuery{ pool, queryIndex, 0u }); return; }
    auto* qp = static_cast<D3D12QueryPool*>(pool);
    if (!m_List || !qp || queryIndex >= qp->GetQueryCount() ||
        qp->GetQueryType() != QueryType::PipelineStatistics) return;
    m_List->EndQuery(qp->GetHeap(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, queryIndex);
    m_List->ResolveQueryData(qp->GetHeap(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, queryIndex, 1,
                             qp->GetReadback(), static_cast<UINT64>(queryIndex) * qp->GetResultStride());
    m_PendingQueries.emplace_back(qp, queryIndex);
}

void D3D12CommandList::ResetQueryPool(IRHIQueryPool* pool) {
    if (m_StreamMode) { Stream().Enqueue(RHICommandType::CmdResetQueryPool, RHICommandQuery{ pool, 0u, 0u }); return; }
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
    if (m_StreamMode) return;   // 流模式下没有可直接录制的原生列表（见 D3D12Device::GetImGuiCommandBuffer）
    if (!m_List || !m_Device || !m_Device->EnsureDescriptorHeaps()) return;
    ID3D12DescriptorHeap* heaps[2] = { m_Device->GetCbvSrvUavHeap(), m_Device->GetSetSamplerHeap() };
    if (heaps[0] && heaps[1]) {
        m_List->SetDescriptorHeaps(2, heaps);
        m_HeapsSet = true;
    }
    m_Pipeline = nullptr;      // 外部会设置自己的 PSO / 根签名
    m_RootSigBound = false;    // 外部渲染会改根签名：之后的根参数下发必须重新等 SetPipeline
    m_RootArgsDirty = true;
}

void D3D12CommandList::End() {
    if (TraceEnabled()) { std::printf("[TRACE] End\n"); std::fflush(stdout); }
    if (m_StreamMode) { m_Recording = false; return; }   // 流模式：回放与 Close 由影子列表完成

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
    if (m_StreamMode) {
        // 后缓冲在渲染线程上确定：记下本帧的序号与当前状态并编码进 renderPass 字段（回放侧经
        // SetRenderPassHint 交还），再按「已转到渲染目标」更新交换链的状态记录（Present 据此做屏障）
        if (!m_SwapChain && m_Device) m_SwapChain = m_Device->GetSwapChainPtr();
        uintptr_t hint = 0;
        if (m_SwapChain) {
            auto* sc = static_cast<D3D12SwapChain*>(m_SwapChain);
            const u32 idx = sc->GetRecordIndex();
            hint = (static_cast<uintptr_t>(sc->GetBackBufferState()) << 32) | (static_cast<uintptr_t>(idx) + 1u);
            sc->MarkBackBufferRendered();
            // 【T10】录制期就把后缓冲登记成 RENDER_TARGET：Present 在渲染线程上读设备状态表，
            // 若等回放（RHI 线程）再登记，Present 可能先看到旧状态 ⇒ 调试层 id=538
            //（PRESENT_SOURCE 要求 COMMON/PRESENT，实际已是 RENDER_TARGET）。
            if (m_Device) {
                if (ID3D12Resource* bb = sc->GetBackBuffer(idx))
                    m_Device->SetResourceState(bb, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
        }
        // 清屏值放 arena（本后端只读 clear[0]）
        RHICommandBeginRenderPass p{};
        p.renderPass = reinterpret_cast<void*>(hint);
        p.colorCount = colorCount; p.colorFormat = static_cast<u32>(colorFormat);
        p.depthFormat = static_cast<u32>(depthFormat); p.loadOp = static_cast<u32>(loadOp);
        if (clear) {
            void* dst = Stream().AllocArena(sizeof(ClearValue), 16);
            std::memcpy(dst, clear, sizeof(ClearValue));
            p.clearOffset = Stream().ArenaOffsetOf(dst);
            p.clearCount  = 1u;
        }
        Stream().Enqueue(RHICommandType::CmdBeginRenderPass, p);
        return;
    }
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
    // 命令流回放：用录制时登记的后缓冲序号与状态（见 m_BackBufferHint），不查交换链当前记录
    const uintptr_t hint = m_BackBufferHint;
    m_BackBufferHint = 0;
    const bool hinted = (hint & 0xFFFFFFFFu) != 0;
    const u32  hintIndex = static_cast<u32>(hint & 0xFFFFFFFFu) - 1u;
    if (m_SwapChain) {
        // 引擎的 IRHISwapChain 只有 D3D12 交换链会走到这里；
        // 用 static_cast 而不是 dynamic_cast，避免依赖 RTTI。
        auto* d3dSc = static_cast<D3D12SwapChain*>(m_SwapChain);
        rtv = hinted ? d3dSc->GetRtv(hintIndex) : d3dSc->GetCurrentRtv();
        hasRtv = rtv.ptr != 0;
    }

    // 【交换链后缓冲必须显式屏障】D3D12 不会把 PRESENT 隐式提升为 RENDER_TARGET，
    // 不做转换就以 RTV 使用会被判非法、渲染内容被丢弃（调试层 id=538 实测 1669 条，
    // 这也是 02.Cube 画面全黑的根因）。
    ID3D12Resource* bb = nullptr;   // 本帧的后缓冲（下面同步状态用，可能为空）
    if (hasRtv && m_SwapChain) {
        auto* sc = static_cast<D3D12SwapChain*>(m_SwapChain);
        bb = hinted ? sc->GetBackBuffer(hintIndex) : sc->GetCurrentBackBufferResource();
        // 【T9：状态以设备表为准】后缓冲的 RTV 已登记进设备状态表，真实状态从这里取；
        // 命令流回放用录制时登记的 hint（回放线程不能读渲染线程正在改的记录）
        const D3D12_RESOURCE_STATES bbState = hinted
            ? static_cast<D3D12_RESOURCE_STATES>(static_cast<u64>(hint) >> 32)
            : (m_Device && bb ? m_Device->GetResourceState(bb) : sc->GetBackBufferState());
        if (bb && bbState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            D3D12_RESOURCE_BARRIER tb{};
            tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            tb.Transition.pResource   = bb;
            tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            tb.Transition.StateBefore = bbState;
            tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            m_List->ResourceBarrier(1, &tb);
            // 【T10】回放列表不再写设备状态表（同一资源的后续帧录制也写它，回放写会覆盖更新的状态）
            if (m_Device && !m_StreamReplay) m_Device->SetResourceState(bb, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    }

    m_List->OMSetRenderTargets(colorCount, hasRtv ? &rtv : nullptr, FALSE, nullptr);
    m_HasRtv = hasRtv;
    // 告诉交换链：后缓冲现在处于 RENDER_TARGET 状态（截图做屏障时要用真实状态）。
    // 回放时这一步已在录制侧（渲染线程）做过，交换链的状态记录只归渲染线程写
    if (hasRtv && m_SwapChain && !hinted) {
        static_cast<D3D12SwapChain*>(m_SwapChain)->MarkBackBufferRendered();
        if (m_Device && bb && !m_StreamReplay) m_Device->SetResourceState(bb, D3D12_RESOURCE_STATE_RENDER_TARGET);
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
    if (m_StreamMode) {
        Stream().Enqueue(RHICommandType::CmdEndRenderPass, u32{0});
        // 【T10】录制期的附件表也必须随 pass 结束清空：否则后续绘制会把这些资源误判为"正作为附件"，
        // 跳过它们的资源状态转换（原生路径在下面同样会清）。
        ClearPassAttachments();
        return;
    }
    if (TraceEnabled()) { std::printf("[TRACE] EndRenderPass\n"); std::fflush(stdout); }

    // D3D12 没有 pass 结束的概念；资源状态转换由显式 PipelineBarrier 负责
    ClearPassAttachments();
}

void D3D12CommandList::BeginOffscreenPass(void* colorImageView, void* depthImageView,
                                          u32 width, u32 height,
                                          const ClearValue* clear,
                                          bool allowSecondary) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ToCpuHandle(colorImageView);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ToCpuHandle(depthImageView);
    D3D12_CPU_DESCRIPTOR_HANDLE* rtvPtr = colorImageView ? &rtv : nullptr;
    const u32 rtvCount = colorImageView ? 1u : 0u;
    D3D12_CPU_DESCRIPTOR_HANDLE* dsvPtr = depthImageView ? &dsv : nullptr;

    if (m_StreamMode) { // 清屏值放 arena（本后端只读 clear 的第一个元素：颜色与深度都在里面）
        // 【T10】附件状态转换必须在录制期解析：模式 2 下回放时设备状态表已属于后面帧，
        // 回放侧再查会把 RTV/DSV 转到错误的状态（T10 挂起的根因之一）。
        ResolveAttachmentTransitions(rtvPtr, rtvCount, dsvPtr);
        RHICommandBeginOffscreenPass p{};
        p.colorImageView = colorImageView; p.depthImageView = depthImageView;
        p.width = width; p.height = height; p.allowSecondary = allowSecondary ? 1u : 0u;
        if (clear) {
            void* dst = Stream().AllocArena(sizeof(ClearValue), 16);
            std::memcpy(dst, clear, sizeof(ClearValue));
            p.clearOffset = Stream().ArenaOffsetOf(dst);
            p.clearCount  = 1u;
        }
        Stream().Enqueue(RHICommandType::CmdBeginOffscreenPass, p);
        return;
    }
    // 【T10】回放：状态转换已随命令流完成（上面的录制期解析已编码 CmdResolvedBarrier），
    // 这里只做纯绑定：RTV/DSV、按 PSO loadOp 的清屏、视口与裁剪。
    if (m_StreamReplay) {
        if (!m_List) return;
        SetPassAttachments(rtvPtr, rtvCount, dsvPtr);
        BindPassTargets(rtvPtr, rtvCount, dsvPtr, width, height, clear);
        return;
    }
    (void)allowSecondary;
    if (!m_List) return;

    // 记下本 pass 的附件并完成状态转换（D3D12 要求 RTV/DSV 处于 RENDER_TARGET / DEPTH_WRITE；
    // 只有深度附件的 pass 传 1+nullptr 是非法调用，这里按指针是否为空决定 RTV 数量）
    ResolveAttachmentTransitions(rtvPtr, rtvCount, dsvPtr);

    // 【HDR 中间结果转储】进入宽屏离屏 pass 时，把该目标回读并打印首像素。
    // 此刻里面是**上一帧**完整渲染后的结果，可判断几何是否真的画进了 HDR。
    // 【任意尺寸的渲染目标回读扫描】HE_DUMP_HDR=1 时，把每个离屏 pass 的目标
    // 拷到回读缓冲，下一帧等 GPU 完成后扫描整张图，统计非零像素与最大值。
    // 用途：定位"哪个 pass 真正产出了内容"，从而找出黑屏发生在哪一步。
    static const bool s_DumpHdr = (std::getenv("HE_DUMP_HDR") != nullptr);   // 只读一次环境变量
    if (s_DumpHdr && colorImageView && m_Device && m_Device->GetDevice() && m_Device->GetQueue()) {
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

    BindPassTargets(rtvPtr, rtvCount, dsvPtr, width, height, clear);
}

void D3D12CommandList::BeginOffscreenPassMRT(void* const* colorImageViews, u32 colorCount,
                                             void* depthImageView, u32 width, u32 height,
                                             const ClearValue* clears,
                                             bool allowSecondary) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8]{};
    const u32 n = (colorCount < 8) ? colorCount : 8;
    for (u32 i = 0; i < n; ++i) rtvs[i] = ToCpuHandle(colorImageViews ? colorImageViews[i] : nullptr);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ToCpuHandle(depthImageView);
    D3D12_CPU_DESCRIPTOR_HANDLE* dsvPtr = depthImageView ? &dsv : nullptr;

    if (m_StreamMode) { // 附件视图数组与清屏值都放 arena（本后端读 clears[0 .. colorCount)，深度取 clears[0]）
        // 【T10】同 BeginOffscreenPass：附件状态转换在录制期解析并入流
        ResolveAttachmentTransitions(n ? rtvs : nullptr, n, dsvPtr);
        RHICommandBeginOffscreenPassMRT p{};
        p.depthImageView = depthImageView; p.colorCount = colorCount;
        p.width = width; p.height = height; p.allowSecondary = allowSecondary ? 1u : 0u;
        if (colorCount && colorImageViews) {
            void* dst = Stream().AllocArena(sizeof(void*) * colorCount, 16);
            std::memcpy(dst, colorImageViews, sizeof(void*) * colorCount);
            p.colorViewsOffset = Stream().ArenaOffsetOf(dst);
        }
        if (clears) {
            const u32 cn = colorCount ? colorCount : 1u;
            void* dst = Stream().AllocArena(sizeof(ClearValue) * cn, 16);
            std::memcpy(dst, clears, sizeof(ClearValue) * cn);
            p.clearOffset = Stream().ArenaOffsetOf(dst);
            p.clearCount  = cn;
        }
        Stream().Enqueue(RHICommandType::CmdBeginOffscreenPassMRT, p);
        return;
    }
    // 【T10】回放：状态转换已随命令流完成，只做纯绑定
    if (m_StreamReplay) {
        if (!m_List || colorCount == 0) return;
        SetPassAttachments(rtvs, n, dsvPtr);
        BindPassTargets(rtvs, n, dsvPtr, width, height, clears);
        return;
    }
    (void)allowSecondary;
    if (!m_List || colorCount == 0) return;

    // 【MRT 也必须做状态转换】此前只有单目标路径做了屏障，多目标 pass（GBuffer 等）
    // 的渲染目标停在 COMMON/PRESENT，被调试层判定非法、绘制被丢弃（id=538）。
    ResolveAttachmentTransitions(rtvs, n, dsvPtr);
    BindPassTargets(rtvs, n, dsvPtr, width, height, clears);
}

// 绑定 RTV/DSV + 按 PSO 的 loadOp 清屏 + 设置整张目标的视口与裁剪。
// 原生录制与命令流回放共用：回放侧的状态转换已经在录制期解析进命令流，这里只做纯绑定。
void D3D12CommandList::BindPassTargets(const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, u32 rtvCount,
                                       const D3D12_CPU_DESCRIPTOR_HANDLE* dsv,
                                       u32 width, u32 height, const ClearValue* clears) {
    if (!m_List) return;
    const bool hasDsv = dsv != nullptr && dsv->ptr != 0;
    const u32 n = (rtvCount < 8u) ? rtvCount : 8u;
    m_List->OMSetRenderTargets(n, (n && rtvs) ? rtvs : nullptr, FALSE, hasDsv ? dsv : nullptr);
    m_HasRtv = n > 0;

    // 【是否清空附件由 PSO 的 loadOp 决定】与 Vulkan 一致：离屏 pass 用当前 PSO 的 render pass，
    // Clear 才清空、Load 保留；clear 只提供清屏值（为空时颜色 0、深度 1.0）。此前深度无条件清空，
    // 延迟管线的天空盒 pass（depthLoadOp=Load）因此丢掉 GBuffer 深度、覆盖整张光照结果。
    const bool gfxPso     = m_Pipeline && !m_IsComputePipeline;
    const bool clearColor = gfxPso ? (m_Pipeline->GetColorLoadOp() == LoadOp::Clear) : (clears != nullptr);
    const bool clearDepth = gfxPso ? (m_Pipeline->GetDepthLoadOp() == LoadOp::Clear) : (clears != nullptr);
    static const float kZeroColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (clearColor) {
        for (u32 i = 0; i < n; ++i) {
            if (!rtvs || !rtvs[i].ptr) continue;
            m_List->ClearRenderTargetView(rtvs[i], clears ? clears[i].color : kZeroColor, 0, nullptr);
        }
    }
    if (hasDsv && clearDepth) {
        // 【只有带模板的格式才能传 STENCIL 标志】D32_FLOAT 没有模板位，
        // 同时传 DEPTH|STENCIL 会让整个 ClearDepthStencilView 被拒，
        // 深度缓冲留在未初始化状态 ⇒ 所有物体（含天空盒）都过不了深度测试 ⇒ 画面全黑。
        D3D12_CLEAR_FLAGS dflags = D3D12_CLEAR_FLAG_DEPTH;
        if (m_Device) {
            if (ID3D12Resource* dres = m_Device->FindViewResource(static_cast<UINT64>(dsv->ptr))) {
                const DXGI_FORMAT df = dres->GetDesc().Format;
                if (df == DXGI_FORMAT_D24_UNORM_S8_UINT || df == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
                    dflags |= D3D12_CLEAR_FLAG_STENCIL;
            }
        }
        m_List->ClearDepthStencilView(*dsv, dflags,
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
    if (m_StreamMode) {
        Stream().Enqueue(RHICommandType::CmdEndOffscreenPass, u32{0});
        ClearPassAttachments();   // 【T10】同 EndRenderPass：录制期附件表随 pass 结束清空
        return;
    }
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
    if (m_StreamMode) {
        // 【T10】流模式下也要更新本地管线状态：录制期的描述符快照、资源状态解析、
        // 以及 pass begin 的 loadOp 判断都按当前 PSO 反射出来的信息进行。
        m_Pipeline   = static_cast<D3D12PipelineState*>(pso);
        m_RtPipeline = nullptr;
        if (m_Pipeline) m_IsComputePipeline = m_Pipeline->IsCompute();
        m_RootArgsDirty = true;
        Stream().Enqueue(RHICommandType::SetPipeline, RHICommandSetPipeline{ pso });
        return;
    }
    // 计算列表不能设置图形管线（COMPUTE 列表上 SetGraphicsRootSignature 非法）。异步计算 pass
    // 结尾常为主列表「恢复」图形 PSO（如 GPU_Cull 末尾的 SetPipeline(GBuffer PSO)），这里忽略
    if (IsComputeQueueList() && pso && !static_cast<D3D12PipelineState*>(pso)->IsCompute()) return;
    m_Pipeline = static_cast<D3D12PipelineState*>(pso);
    m_RtPipeline = nullptr;   // 切回普通图形 / 计算管线：不再处于光追状态对象绑定
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
        m_RootSigBound = true;   // 之后才允许下发根参数
        m_RootArgsDirty = true;
        return;   // 计算管线不涉及输入装配（计算列表上调用 IA* 非法，调试层 id=932）
    } else {
        m_List->SetGraphicsRootSignature(m_Pipeline->GetRootSignature());
        m_RootSigBound = true;   // 之后才允许下发根参数
    }
    m_List->IASetPrimitiveTopology(m_Pipeline->GetTopology());
    // 引擎布局缺失的属性挂在零缓冲槽上，这里把零缓冲绑上
    if (m_Pipeline->NeedsZeroVertexBuffer() && m_Device) {
        const D3D12_VERTEX_BUFFER_VIEW zv = m_Device->GetZeroVertexBufferView();
        if (zv.BufferLocation) m_List->IASetVertexBuffers(D3D12PipelineState::kZeroVertexSlot, 1, &zv);
    }
    // 【T6】不再绑定实例序号缓冲：顶点着色器用 SV_StartInstanceLocation 取起始实例号
    //（此前这里会把 0,1,2,… 的逐实例序号缓冲绑到 14 号槽，作为 SV_InstanceID 的替代）
    // 设置根签名会清空所有根参数，下一次绘制/派发前需要重新下发
    m_RootArgsDirty = true;
}

void D3D12CommandList::SetVertexBuffer(IRHIBuffer* buffer, u32 binding) {
    if (m_StreamMode) { Stream().Enqueue(RHICommandType::SetVertexBuffer, RHICommandSetVertexBuffer{ buffer, binding, 0u }); return; }
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
    if (IsComputeQueueList()) return;   // 计算列表没有输入装配阶段
    m_List->IASetVertexBuffers(binding, 1, &vbv);
}

void D3D12CommandList::SetIndexBuffer(IRHIBuffer* buffer, u32 offset) {
    if (m_StreamMode) { Stream().Enqueue(RHICommandType::SetIndexBuffer, RHICommandSetIndexBuffer{ buffer, offset, 0u }); return; }
    if (TraceEnabled()) { std::printf("[TRACE] SetIndexBuffer\n"); std::fflush(stdout); }
    if (!m_List || !buffer) return;
    auto* b = static_cast<D3D12Buffer*>(buffer);
    D3D12_INDEX_BUFFER_VIEW ibv = b->GetIbv();
    ibv.BufferLocation += offset;
    if (IsComputeQueueList()) return;   // 计算列表没有输入装配阶段
    m_List->IASetIndexBuffer(&ibv);
}

void D3D12CommandList::SetViewport(const Viewport& vp) {
    if (m_StreamMode) { Stream().Enqueue(RHICommandType::SetViewport, RHICommandSetViewport{ vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth }); return; }
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
    if (m_IsBundle || IsComputeQueueList()) return;   // bundle 继承主列表的视口；计算列表没有光栅阶段
    m_List->RSSetViewports(1, &v);
}

void D3D12CommandList::SetScissor(const ScissorRect& sc) {
    if (m_StreamMode) { Stream().Enqueue(RHICommandType::SetScissor, RHICommandSetScissor{ sc.x, sc.y, sc.width, sc.height }); return; }
    if (TraceEnabled()) { std::printf("[TRACE] SetScissor\n"); std::fflush(stdout); }
    if (!m_List) return;
    D3D12_RECT r{};
    r.left   = sc.x;
    r.top    = sc.y;
    r.right  = sc.x + static_cast<i32>(sc.width);
    r.bottom = sc.y + static_cast<i32>(sc.height);
    if (m_IsBundle || IsComputeQueueList()) return;   // bundle 继承主列表的裁剪；计算列表没有光栅阶段
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
    if (m_StreamMode) {
        // 【T10】录制期也要记下绑定集：绘制前的资源状态解析要按集内容找资源；
        // 描述符表本身在绘制前由 PrepareSetTables 拍快照并入流。
        if (set != kInvalidSet && setIndex < 8) {
            m_BoundSets[setIndex] = set;
            m_LastBoundSet = set;
            if (m_Device) m_LastBaseSlot = m_Device->GetSetBaseSlot(set);
        }
        Stream().Enqueue(RHICommandType::SetDescriptorSet, RHICommandSetDescriptorSet{ setIndex, 0u, static_cast<u64>(set) });
        return;
    }
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

void D3D12CommandList::PrepareBindlessTables() {
    if (!m_Device) return;
    // space2 的 bindless 表：此前只在 DrawIndexed 里设置，计算派发与 Draw 都没有，
    // 用 bindless 的 compute（如逐实例剔除）读到未初始化的根参数（GPU 验证 id=935）
    if (const auto h = m_Device->GetBindlessSrvGpuHandle(); h.ptr) SetRootTable(10, h);
    if (const auto h = m_Device->GetBindlessUavGpuHandle(); h.ptr) SetRootTable(18, h);
    if (const auto h = m_Device->GetBindlessSmpGpuHandle(); h.ptr) SetRootTable(26, h);
}

void D3D12CommandList::PrepareSetTables() {
    if (!m_Device) return;
    PrepareBindlessTables();
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
        // 【P3 诊断】快照落到哪个槽位、该 set 里的 AS 型 binding 写的是什么地址
        if (StreamRTDebugEnabled()) {
            static u32 s_dbgSnap = 0;
            if (++s_dbgSnap <= 16u || (s_dbgSnap % 120u) == 0u) {
                const auto& asb = m_Device->GetSetAsBindings(set);
                HE_CORE_INFO("[RTDBG] 描述符快照：set={} ver={} baseSlot={} srvSlot={} uavSlot={} "
                             "chunk={} asBinds={} stream={}",
                             static_cast<u64>(set), ver, m_Device->GetSetBaseSlot(set),
                             DbgDescSlot(m_Device, srv), DbgDescSlot(m_Device, uav), chunk,
                             asb.size(), m_StreamMode ? 1 : 0);
                for (const auto& [b, addr] : asb)
                    HE_CORE_INFO("[RTDBG]    AS binding {} -> addr=0x{:x}", b, addr);
            }
        }
        // 【T10】快照必须随命令流携带：模式 2 下回放落后一到两帧，回放时再拍快照拿到的
        // 是后面帧写入的内容（描述符集不按帧分版本 ⇒ 实测挂起）。这里把录制当时的 GPU 句柄入流。
        if (m_StreamMode) {
            RHICommandDescriptorTables t{};
            t.setIndex = i;
            t.set      = static_cast<u64>(set);
            t.srv      = srv.ptr;
            t.uav      = uav.ptr;
            t.sampler  = m_Device->GetSetSamplerGpuHandle(m_Device->GetSetSamplerBase(set)).ptr;
            Stream().Enqueue(RHICommandType::CmdResolvedDescriptorTables, t);
        }
    }
}

// ---------------------------------------------------------------------------
//  【T10】录制期解析结果的发送端
//
//  普通列表：直接下原生屏障；命令流录制：编码成一条 CmdResolvedBarrier（回放只按数据下发）；
//  命令流回放：不会走到这里（回放列表跳过所有状态解析）。
// ---------------------------------------------------------------------------
void D3D12CommandList::EmitBarrier(const D3D12_RESOURCE_BARRIER& barrier) {
    if (m_StreamMode) {
        RHICommandResolvedBarrier r{};
        switch (barrier.Type) {
        case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
            r.resource    = barrier.Transition.pResource;
            r.type        = static_cast<u32>(ResolvedBarrierType::Transition);
            r.subresource = barrier.Transition.Subresource;
            r.before      = static_cast<u32>(barrier.Transition.StateBefore);
            r.after       = static_cast<u32>(barrier.Transition.StateAfter);
            break;
        case D3D12_RESOURCE_BARRIER_TYPE_UAV:
            r.resource = barrier.UAV.pResource;
            r.type     = static_cast<u32>(ResolvedBarrierType::Uav);
            break;
        case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
            r.resource = barrier.Aliasing.pResourceAfter;
            r.type     = static_cast<u32>(ResolvedBarrierType::Aliasing);
            break;
        default:
            return;
        }
        Stream().Enqueue(RHICommandType::CmdResolvedBarrier, r);
        return;
    }
    if (m_List) m_List->ResourceBarrier(1, &barrier);
}

void D3D12CommandList::EmitDiscard(ID3D12Resource* res) {
    if (!res) return;
    if (m_StreamMode) {
        RHICommandResolvedBarrier r{};
        r.resource = res;
        r.type     = static_cast<u32>(ResolvedBarrierType::Discard);
        Stream().Enqueue(RHICommandType::CmdResolvedBarrier, r);
        return;
    }
    if (m_List) m_List->DiscardResource(res, nullptr);
}

void D3D12CommandList::EmitResolvedRenderTargets(const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, u32 count,
                                                 const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    const u32 n = count ? count : 0u;
    if (m_StreamMode) {
        RHICommandResolvedRenderTargets p{};
        p.count = n;
        p.dsv   = dsv ? reinterpret_cast<void*>(dsv->ptr) : nullptr;
        if (n) {
            void* dst = Stream().AllocArena(sizeof(void*) * n, 16);
            for (u32 i = 0; i < n; ++i) {
                static_cast<void**>(dst)[i] = rtvs ? reinterpret_cast<void*>(rtvs[i].ptr) : nullptr;
            }
            p.rtvsOffset = Stream().ArenaOffsetOf(dst);
        }
        Stream().Enqueue(RHICommandType::CmdResolvedRenderTargets, p);
        return;
    }
    if (m_List) m_List->OMSetRenderTargets(n, (n && rtvs) ? rtvs : nullptr, FALSE, dsv);
}

void D3D12CommandList::ResolveAttachmentTransitions(const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, u32 rtvCount,
                                                    const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    if (!m_Device) return;
    // 附件表：绘制前补纹理状态时要跳过正在作为附件的资源
    SetPassAttachments(rtvs, rtvCount, dsv);
    for (u32 i = 0; i < rtvCount; ++i) {
        if (!rtvs[i].ptr) continue;
        ID3D12Resource* res = m_Device->FindViewResource(static_cast<UINT64>(rtvs[i].ptr));
        if (!res) continue;
        ActivateAliasedTexture(res);   // 瞬态纹理首次使用：先激活
        const D3D12_RESOURCE_STATES from = m_Device->GetResourceState(res);
        if (from == D3D12_RESOURCE_STATE_RENDER_TARGET) continue;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        EmitBarrier(b);
        m_Device->SetResourceState(res, D3D12_RESOURCE_STATE_RENDER_TARGET);
        SyncBackBufferStateForAttachment(res);   // 附件是交换链后缓冲时同步那份镜像
    }
    if (dsv && dsv->ptr) {
        ID3D12Resource* res = m_Device->FindViewResource(static_cast<UINT64>(dsv->ptr));
        if (res) {
            ActivateAliasedTexture(res);
            const D3D12_RESOURCE_STATES from = m_Device->GetResourceState(res);
            if (from != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource   = res;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = from;
                b.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                EmitBarrier(b);
                m_Device->SetResourceState(res, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            }
            SyncBackBufferStateForAttachment(res);
        }
    }
}

void D3D12CommandList::AfterDrawOrDispatch() {
    if (m_PostDrawBarriers.empty()) return;
    for (const auto& b : m_PostDrawBarriers) EmitBarrier(b);
    m_PostDrawBarriers.clear();
}

// 【T10】流模式：绘制/派发前的录制期解析。顺序与原生路径一致（屏障 → 描述符表 → 绘制命令）
void D3D12CommandList::StreamDrawPrologue() {
    EnsureBoundResourceStates();
    PrepareSetTables();
}

void D3D12CommandList::StreamDrawEpilogue() {
    AfterDrawOrDispatch();   // 绘制后屏障：命令流里排在绘制命令之后
    m_PushDirty = true;
}

void D3D12CommandList::EnsureBoundResourceStates() {
    // 【T10】回放侧不再解析：模式 2 下回放落后录制线程一到两帧，这里查描述符集内容 / 设备状态表
    // 拿到的是后面帧的数据（屏障 StateBefore 错位、转换到错误资源）⇒ GPU 挂起。录制期已把结果入流。
    if (m_StreamReplay) return;
    // 光追状态对象没有普通 PSO，但仍要按库反射出来的资源用途补状态转换 ⇒ 允许 m_RtPipeline
    // （流模式下 m_List 不参与录制，屏障经 EmitBarrier 编码进命令流）
    if (!m_Device || (!m_Pipeline && !m_RtPipeline)) return;
    if (m_IsBundle) {
        // bundle 不能发屏障：记下本次绘制的管线与绑定集（与上一条相同则不重复），由主列表执行前补转换
        BundleUsage u;
        u.pipeline = m_Pipeline;
        u.compute  = m_IsComputePipeline;
        std::copy(std::begin(m_BoundSets), std::end(m_BoundSets), std::begin(u.sets));
        if (m_BundleUsages.empty() || m_BundleUsages.back().pipeline != u.pipeline ||
            !std::equal(std::begin(u.sets), std::end(u.sets), std::begin(m_BundleUsages.back().sets)))
            m_BundleUsages.push_back(u);
        return;
    }
    // 可读状态按队列取：计算列表上不能出现 PIXEL_SHADER_RESOURCE（见 ShaderReadState）
    const D3D12_RESOURCE_STATES kRead = ShaderReadState();
    m_BarrierScratch.clear();
    // 只有真正的图形 PSO 才涉及渲染目标 / 深度附件（光追按计算管线的口径处理）
    const bool gfxPso = m_Pipeline && !m_IsComputePipeline;

    // 资源用途表：普通 PSO 用 DXIL 反射，光追状态对象用库反射，二者同一口径
    const std::vector<D3D12ResourceUse>* uses = nullptr;
    if (m_RtPipeline)      uses = &m_RtPipeline->GetResourceUses();
    else if (m_Pipeline)   uses = &m_Pipeline->GetResourceUses();
    if (!uses) return;
    // 光追且库反射不可用：退回按描述符集里记录的类型保守转换（RT 里写 SSBO 的场景极少）
    if (m_RtPipeline && uses->empty()) {
        EnsureRTSetStatesByType();
        for (const auto& b : m_BarrierScratch) EmitBarrier(b);
        return;
    }

    // 当前绑着只读 DSV，但新的 PSO 要写深度：先切回可写 DSV
    if (m_DsvReadOnly && gfxPso && m_PassDsvRes && m_Pipeline->GetDepthWrite()) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = m_PassDsvRes;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = m_Device->GetResourceState(m_PassDsvRes);
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        EmitBarrier(b);
        m_Device->SetResourceState(m_PassDsvRes, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        EmitResolvedRenderTargets(m_PassRtvs, m_PassRtvCount, &m_PassDsv);
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
        for (const auto& use : *uses) {
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
            const D3D12_RESOURCE_STATES cur = UsableTextureState(res);   // 计算列表：图形专用状态先交接
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

    for (const auto& use : *uses) {
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
        const D3D12_RESOURCE_STATES cur  = isBuffer ? GetBufferState(res) : UsableTextureState(res);
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
    if (!m_BarrierScratch.empty()) {
        for (const auto& b : m_BarrierScratch) EmitBarrier(b);
    }
    if (roDsv) {
        D3D12_CPU_DESCRIPTOR_HANDLE ro{};
        ro.ptr = static_cast<SIZE_T>(roDsv);
        EmitResolvedRenderTargets(m_PassRtvs, m_PassRtvCount, &ro);
        m_DsvReadOnly = true;
    }
}

void D3D12CommandList::SetRootTable(u32 param, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
    if (param >= 32u) return;
    if (m_RootTables[param].ptr == handle.ptr) return;   // 未变：不必重发（切管线另有标脏）
    m_RootTables[param] = handle;
    m_RootArgsDirty = true;
}

void D3D12CommandList::BufferBarrier(IRHIBuffer* buffer, ResourceState srcState, ResourceState dstState) {
    // 【T10】回放侧不再解析缓冲状态（录制期已把转换编码进命令流）
    if (m_StreamReplay) return;
    if (!buffer) return;
    // 流模式下 TransitionBuffer 把屏障编码进命令流；普通列表直接下原生屏障
    TransitionBuffer(static_cast<D3D12Buffer*>(buffer)->GetResource(), ToD3D12ResourceState(dstState, false));
}

void D3D12CommandList::FlushRootArgs() {
    EmitDrawLabel();      // 逐绘制标签：插在本次绘制 / 派发的命令之前
    // 集内容变了要换快照：会更新 SRV/UAV 表并标脏根参数。
    // 【T10】回放列表不再拍快照（那已是后面帧的内容），描述符表由命令流携带；
    // bindless 表的句柄由设备常量决定，回放侧可以安全重算。
    if (m_StreamReplay) PrepareBindlessTables();
    else                PrepareSetTables();
    if (!m_List || (!m_Pipeline && !m_RtPipeline) || !m_RootArgsDirty) return;
    // 【防静默挂起】D3D12 在没有绑定根签名时调用 SetGraphics/SetComputeRoot* 会直接卡死
    //（CPU 0%、无调试层报错）。命令流回退帧（HE_RHI_COMMAND_STREAM_RT=0 命中光追）会跳过
    // 光追管线绑定，但后续的推送常量 / 绘制仍会走到这里 ⇒ 没有根签名就什么都不下发。
    // 根参数保持"脏"，等真正绑了根签名的那次 FlushRootArgs 再补。
    if (!m_RootSigBound) return;
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
            // 光追的 closesthit 会读 b1：计算/光追路径同样要绑，否则 DXR 报
            // "Uninitialized root argument accessed (Root Parameter Index: [1])" 并挂起
            m_List->SetComputeRootConstantBufferView(1, m_PushGpu);
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
    if (m_StreamMode) { // 调用方的数据可能在栈上：拷进命令流 arena
        RHICommandSetPushConstants pc{};
        pc.size = size; pc.offset = offset;
        if (size && data) {
            void* dst = Stream().AllocArena(size, 16);
            std::memcpy(dst, data, size);
            pc.arenaOffset = Stream().ArenaOffsetOf(dst);
            Stream().Enqueue(RHICommandType::SetPushConstants, pc);
        }
        return;
    }
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
        // 【防静默挂起】没有根签名时不许下发根参数（D3D12 会卡死，见 m_RootSigBound 的说明）；
        // 命令流回退帧里光追管线绑定被跳过，推送常量仍会到达这里。数据照常写进常量环，
        // 只是暂时不绑定；真正绑了根签名后的绘制会重新下发根参数。
        if (m_RootSigBound) {
            if (m_IsComputePipeline) {
                m_List->SetComputeRootConstantBufferView(0, slot);
            } else {
                // 生成的 HLSL 里 FrameConstants 在 register(b0)/(b1)，两者都绑同一块
                m_List->SetGraphicsRootConstantBufferView(0, slot);
                m_List->SetGraphicsRootConstantBufferView(1, slot);
            }
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
    if (m_StreamMode) {
        // 【T10】录制期先解析资源状态与描述符表，再把绘制命令入流（回放跳过解析）
        StreamDrawPrologue();
        Stream().Enqueue(RHICommandType::CmdDraw, RHICommandDraw{ vertexCount, instanceCount, firstVertex, firstInstance });
        StreamDrawEpilogue();
        return;
    }
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
    if (m_StreamMode) {
        StreamDrawPrologue();
        Stream().Enqueue(RHICommandType::DrawIndexed, RHICommandDrawIndexed{ indexCount, firstIndex, vertexOffset, instanceCount, firstInstance });
        StreamDrawEpilogue();
        return;
    }
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
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH: want = sizeof(D3D12_DISPATCH_MESH_ARGUMENTS); break;
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
    if (m_StreamMode) {
        if (auto* b = static_cast<D3D12Buffer*>(buffer))
            TransitionBuffer(b->GetResource(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        StreamDrawPrologue();
        Stream().Enqueue(RHICommandType::CmdDrawIndexedIndirect,
                         RHICommandDrawIndexedIndirect{ buffer, offset, drawCount, stride });
        StreamDrawEpilogue();
        return;
    }
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
    if (m_StreamMode) {
        if (auto* b = static_cast<D3D12Buffer*>(buffer))
            TransitionBuffer(b->GetResource(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        if (auto* cb = static_cast<D3D12Buffer*>(countBuffer))
            TransitionBuffer(cb->GetResource(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        StreamDrawPrologue();
        Stream().Enqueue(RHICommandType::CmdDrawIndexedIndirectCount,
                         RHICommandDrawIndexedIndirectCount{ buffer, countBuffer, offset, countOffset, maxDrawCount, stride });
        StreamDrawEpilogue();
        return;
    }
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

// ---------------------------------------------------------------------------
//  ExecuteGeneratedCommands —— DGC 的等价执行（对应 vkCmdExecuteGeneratedCommandsEXT）
//  desc 里的布局 / 执行集来自 D3D12Device::GetDGCLayout / GetDGCExecutionSet（命令签名 / 管线），
//  序列与计数缓冲以 GPU 地址给出，这里反查成资源 + 偏移后调用带计数缓冲的 ExecuteIndirect。
//  序列数取 min(maxSequenceCount, maxDrawCount)：Vulkan 的 maxDrawCount 也是本次绘制数的上限。
// ---------------------------------------------------------------------------
void D3D12CommandList::ExecuteGeneratedCommands(const DGCExecuteDesc& desc) {
    if (m_StreamMode) {
        // 【T10】录制期把序列 / 计数缓冲转到 INDIRECT_ARGUMENT（回放侧不再解析状态）
        if (m_Device) {
            u64 seqOffset = 0, countOffset = 0;
            if (ID3D12Resource* seqRes = m_Device->FindBufferByAddress(desc.sequencesBufferAddr, seqOffset))
                TransitionBuffer(seqRes, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            if (desc.sequenceCountAddr) {
                if (ID3D12Resource* countRes = m_Device->FindBufferByAddress(desc.sequenceCountAddr, countOffset))
                    TransitionBuffer(countRes, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            }
        }
        RHICommandExecuteGeneratedCommands p{};
        // 【T10】录制期按执行集管线解析描述符表与资源状态：回放侧走 ExecuteGeneratedCommands 的
        // 原生路径（内部是 ExecuteIndirect），它依赖的根参数必须与录制时一致。
        if (auto* pso = static_cast<IRHIPipelineState*>(desc.indirectExecutionSet)) SetPipeline(pso);
        StreamDrawPrologue();
        p.indirectCommandsLayout = desc.indirectCommandsLayout; p.indirectExecutionSet = desc.indirectExecutionSet;
        p.sequencesBufferAddr = desc.sequencesBufferAddr;       p.sequenceCountAddr = desc.sequenceCountAddr;
        p.preprocessBufferAddr = desc.preprocessBufferAddr;     p.preprocessBufferSize = desc.preprocessBufferSize;
        p.maxSequenceCount = desc.maxSequenceCount;             p.maxDrawCount = desc.maxDrawCount;
        Stream().Enqueue(RHICommandType::CmdExecuteGeneratedCommands, p);
        StreamDrawEpilogue();
        return;
    }
    if (!m_List || !m_Device || IsComputeQueueList()) return;
    auto* sig = static_cast<ID3D12CommandSignature*>(desc.indirectCommandsLayout);
    auto* pso = static_cast<IRHIPipelineState*>(desc.indirectExecutionSet);
    if (!sig || !pso || sig != m_Device->GetDGCLayout()) {
        HE_CORE_WARN("D3D12CommandList::ExecuteGeneratedCommands: DGC 未初始化或句柄不匹配，已忽略");
        return;
    }
    u64 seqOffset = 0, countOffset = 0;
    ID3D12Resource* seqRes   = m_Device->FindBufferByAddress(desc.sequencesBufferAddr, seqOffset);
    ID3D12Resource* countRes = desc.sequenceCountAddr
                             ? m_Device->FindBufferByAddress(desc.sequenceCountAddr, countOffset) : nullptr;
    if (!seqRes || (desc.sequenceCountAddr && !countRes)) {
        HE_CORE_WARN("D3D12CommandList::ExecuteGeneratedCommands: 找不到序列 / 计数缓冲（地址未登记），已忽略");
        return;
    }
    u32 maxCount = desc.maxSequenceCount;
    if (desc.maxDrawCount != 0 && desc.maxDrawCount < maxCount) maxCount = desc.maxDrawCount;
    if (maxCount == 0) return;

    if (m_Pipeline != pso) SetPipeline(pso);   // 执行集只有一条管线：相当于 EXECUTION_SET 令牌
    // 序列 / 计数缓冲由 GPU 剔除以 UAV 写出：执行前转到 INDIRECT_ARGUMENT
    // 【T10】回放侧跳过：录制期已把转换编码进命令流
    if (!m_StreamReplay) {
        TransitionBuffer(seqRes, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        if (countRes) TransitionBuffer(countRes, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }
    EnsureBoundResourceStates();
    FlushRootArgs();
    m_List->ExecuteIndirect(sig, maxCount, seqRes, seqOffset, countRes, countOffset);
    AfterDrawOrDispatch();
    m_PushDirty = true;   // 同 Draw：常量槽已被本次绘制消费
}

// ---------------------------------------------------------------------------
//  网格着色器绘制（对应 Vulkan 的 vkCmdDrawMeshTasks*EXT）
//  与普通绘制同一套前置：补资源状态、下发根参数；之后标记常量槽已消费
// ---------------------------------------------------------------------------
void D3D12CommandList::DrawMeshTasks(u32 groupCountX, u32 groupCountY, u32 groupCountZ) {
    if (m_StreamMode) {
        StreamDrawPrologue();
        Stream().Enqueue(RHICommandType::CmdDrawMeshTasks, RHICommandDrawMeshTasks{ groupCountX, groupCountY, groupCountZ, 0u });
        StreamDrawEpilogue();
        return;
    }
    if (!m_List || !m_List6 || IsComputeQueueList()) return;
    if (!m_Pipeline || !m_Pipeline->IsMesh()) {
        HE_CORE_WARN("D3D12CommandList::DrawMeshTasks: 当前未绑定网格管线，已忽略");
        return;
    }
    EnsureBoundResourceStates();
    FlushRootArgs();
    m_List6->DispatchMesh(groupCountX, groupCountY, groupCountZ);
    AfterDrawOrDispatch();
    m_PushDirty = true;   // 同 Draw：常量槽已被本次绘制消费
}

void D3D12CommandList::DrawMeshTasksIndirect(IRHIBuffer* buffer, u64 offset, u32 drawCount, u32 stride) {
    if (m_StreamMode) {
        if (auto* pb = static_cast<D3D12Buffer*>(buffer))
            TransitionBuffer(pb->GetResource(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        StreamDrawPrologue();
        Stream().Enqueue(RHICommandType::CmdDrawMeshTasksIndirect, RHICommandDrawIndexedIndirect{ buffer, offset, drawCount, stride });
        StreamDrawEpilogue();
        return;
    }
    if (!m_List || !m_List6 || !buffer || IsComputeQueueList()) return;
    if (!m_Pipeline || !m_Pipeline->IsMesh()) {
        HE_CORE_WARN("D3D12CommandList::DrawMeshTasksIndirect: 当前未绑定网格管线，已忽略");
        return;
    }
    auto* b = static_cast<D3D12Buffer*>(buffer);
    ID3D12CommandSignature* sig = GetCommandSignature(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH, stride);
    if (!sig) return;
    TransitionBuffer(b->GetResource(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);   // 参数常由计算着色器写出
    EnsureBoundResourceStates();
    FlushRootArgs();
    m_List->ExecuteIndirect(sig, drawCount, b->GetResource(), offset, nullptr, 0);
    AfterDrawOrDispatch();
    m_PushDirty = true;
}

void D3D12CommandList::DrawMeshTasksIndirectCount(IRHIBuffer* buffer, u64 offset, IRHIBuffer* countBuffer,
                                                  u64 countOffset, u32 maxDrawCount, u32 stride) {
    if (m_StreamMode) {
        if (auto* pb = static_cast<D3D12Buffer*>(buffer))
            TransitionBuffer(pb->GetResource(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        if (auto* pcb = static_cast<D3D12Buffer*>(countBuffer))
            TransitionBuffer(pcb->GetResource(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        StreamDrawPrologue();
        Stream().Enqueue(RHICommandType::CmdDrawMeshTasksIndirectCount, RHICommandDrawIndexedIndirectCount{ buffer, countBuffer, offset, countOffset, maxDrawCount, stride });
        StreamDrawEpilogue();
        return;
    }
    if (!m_List || !m_List6 || !buffer || !countBuffer || IsComputeQueueList()) return;
    if (!m_Pipeline || !m_Pipeline->IsMesh()) {
        HE_CORE_WARN("D3D12CommandList::DrawMeshTasksIndirectCount: 当前未绑定网格管线，已忽略");
        return;
    }
    auto* b  = static_cast<D3D12Buffer*>(buffer);
    auto* cb = static_cast<D3D12Buffer*>(countBuffer);
    ID3D12CommandSignature* sig = GetCommandSignature(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH, stride);
    if (!sig) return;
    // 参数缓冲与计数缓冲都要处于 INDIRECT_ARGUMENT（同一缓冲兼作两者时只转一次）
    TransitionBuffer(b->GetResource(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    TransitionBuffer(cb->GetResource(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    EnsureBoundResourceStates();
    FlushRootArgs();
    m_List->ExecuteIndirect(sig, maxDrawCount, b->GetResource(), offset, cb->GetResource(), countOffset);
    AfterDrawOrDispatch();
    m_PushDirty = true;
}

void D3D12CommandList::Dispatch(u32 x, u32 y, u32 z) {
    if (m_StreamMode) {
        StreamDrawPrologue();
        Stream().Enqueue(RHICommandType::CmdDispatch, RHICommandDispatch{ x, y, z, 0u });
        StreamDrawEpilogue();
        return;
    }
    if (!m_List) return;
    EnsureBoundResourceStates();
    FlushRootArgs();
    m_List->Dispatch(x, y, z);
    AfterDrawOrDispatch();
    m_PushDirty = true;   // 同 Draw：常量槽已被本次派发消费
}

void D3D12CommandList::DispatchIndirect(IRHIBuffer* buffer, u64 offset) {
    if (m_StreamMode) {
        if (auto* pb = static_cast<D3D12Buffer*>(buffer))
            TransitionBuffer(pb->GetResource(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        StreamDrawPrologue();
        Stream().Enqueue(RHICommandType::CmdDispatchIndirect, RHICommandDispatchIndirect{ buffer, offset });
        StreamDrawEpilogue();
        return;
    }
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

// ===========================================================================
//  光追（T5 / DXR）
//
//  与 Vulkan 的差异（见 docs/计划实现功能/D3D12剩余任务实施指南.md T5.4 / T5.7）：
//    · 加速结构与 scratch 都是普通缓冲：构建前把 scratch 转到 UNORDERED_ACCESS，
//      构建后对加速结构发 UAV 屏障；加速结构本身**永远**停在
//      RAYTRACING_ACCELERATION_STRUCTURE 状态，绝不参与状态转换。
//    · 顶点 / 索引 / 实例缓冲按「构建输入」要求转到 NON_PIXEL_SHADER_RESOURCE
//      （UPLOAD 堆上的缓冲 TransitionBuffer 会跳过：它们恒为 GENERIC_READ，本就只读）。
// ===========================================================================
ID3D12GraphicsCommandList4* D3D12CommandList::GetList4() {
    if (!m_List) return nullptr;
    if (!m_List4) m_List.As(&m_List4);   // 懒取：不支持 DXR 的运行时这里为空
    return m_List4.Get();
}

void D3D12CommandList::BuildBLAS(IRHIAccelerationStructure* blas, IRHIBuffer* scratchBuffer,
                                 const BLASBuildDesc& desc, bool update) {
    if (m_StreamMode && !StreamRTCarryEnabled()) {
        // 未启用流式光追：保持 T10 的行为（命中即回退直接录制，画面自下一帧恢复）
        MarkStreamUnsupported("BLAS 构建");
        return;
    }
    if (m_StreamMode) {
        // 【P3】把 BLAS 构建编码进命令流（此前命中即标记为无法承载、整帧跳过并回退直接录制）。
        // 顺序与原生路径完全一致：构建输入转非像素可读 → scratch 转 UAV → 构建 → AS 的 UAV 屏障。
        // 状态转换由录制期解析成 CmdResolvedBarrier 随流走；几何描述复制进流 arena
        // （RTGeometryDesc 带 std::vector，不能直接放进命令流）。
        if (!blas) return;
        auto* as = static_cast<D3D12AccelerationStructure*>(blas);
        const u32 geoCount = static_cast<u32>(desc.geometries.size());
        if (geoCount == 0) return;
        for (const auto& g : desc.geometries) {
            if (auto* vb = static_cast<D3D12Buffer*>(g.vertexBuffer))
                TransitionBuffer(vb->GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (auto* ib = static_cast<D3D12Buffer*>(g.indexBuffer))
                TransitionBuffer(ib->GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (auto* tb = static_cast<D3D12Buffer*>(g.transformBuffer))
                TransitionBuffer(tb->GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        D3D12_GPU_VIRTUAL_ADDRESS scratchAddr = 0;
        if (auto* sb = static_cast<D3D12Buffer*>(scratchBuffer)) {
            TransitionBuffer(sb->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            scratchAddr = sb->GetGpuAddress();
        }
        if (scratchAddr == 0) {
            HE_CORE_ERROR("BuildBLAS: 缺少 scratch 缓冲（构建被跳过）");
            return;
        }
        void* dst = Stream().AllocArena(sizeof(RHIBLASGeometry) * geoCount, 8);
        auto* geos = static_cast<RHIBLASGeometry*>(dst);
        for (u32 i = 0; i < geoCount; ++i) {
            const RTGeometryDesc& s = desc.geometries[i];
            RHIBLASGeometry& d = geos[i];
            d.vertexBuffer      = s.vertexBuffer;
            d.indexBuffer       = s.indexBuffer;
            d.transformBuffer   = s.transformBuffer;
            d.vertexStride      = s.vertexStride;
            d.vertexFormat      = static_cast<u32>(s.vertexFormat);
            d.indexFormat       = static_cast<u32>(s.indexFormat);
            d.maxVertex         = s.maxVertex;
            d.maxPrimitiveCount = s.maxPrimitiveCount;
            d.type              = static_cast<u32>(s.type);
        }
        RHICommandBuildBLAS p{};
        p.blas           = blas;
        p.scratch        = scratchBuffer;
        p.geometryOffset = Stream().ArenaOffsetOf(dst);
        p.geometryCount  = geoCount;
        p.flags          = static_cast<u32>(desc.flags);
        p.update         = update ? 1u : 0u;
        Stream().EnqueueArena(RHICommandType::CmdBuildBLAS, p);
        // 构建写入完成后要发 AS 的 UAV 屏障（同原生路径第 5 步）
        D3D12_RESOURCE_BARRIER ub{};
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = as->GetResource();
        EmitBarrier(ub);
        static bool s_LoggedBLASStream = false;
        if (!s_LoggedBLASStream) {
            s_LoggedBLASStream = true;
            HE_CORE_INFO("[D3D12] 命令流承载光追：BLAS 构建已编码进流（{} 个几何）", geoCount);
        }
        return;
    }
    if (!m_List || !blas) return;
    auto* as = static_cast<D3D12AccelerationStructure*>(blas);
    ID3D12GraphicsCommandList4* list4 = GetList4();
    if (!list4) {
        HE_CORE_ERROR("BuildBLAS: 当前命令列表不支持 DXR（ID3D12GraphicsCommandList4）");
        return;
    }

    // 1. 几何描述：引擎的 RTGeometryDesc → D3D12 三角形几何
    const u32 geoCount = static_cast<u32>(desc.geometries.size());
    if (geoCount == 0) return;
    // 【P3 诊断】HE_D3D12_STREAM_RT_DBG=1 时确认回放侧真的执行了原生构建
    {
        static const bool s_dbg = (std::getenv("HE_D3D12_STREAM_RT_DBG") != nullptr);
        static u32 s_n = 0;
        if (s_dbg && ++s_n <= 8u) {
            HE_CORE_INFO("[RTDBG] 原生 BuildBLAS 执行：replay={} 几何={} update={}",
                         m_StreamReplay ? 1 : 0, geoCount, update ? 1 : 0);
        }
    }
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geos(geoCount);
    for (u32 i = 0; i < geoCount; ++i) FillD3D12GeometryDesc(desc.geometries[i], geos[i]);

    // 2. 构建输入缓冲 → 非像素着色器可读（AS 构建按 SRV 读取顶点 / 索引）
    for (const auto& g : desc.geometries) {
        if (auto* vb = static_cast<D3D12Buffer*>(g.vertexBuffer))
            TransitionBuffer(vb->GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (auto* ib = static_cast<D3D12Buffer*>(g.indexBuffer))
            TransitionBuffer(ib->GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (auto* tb = static_cast<D3D12Buffer*>(g.transformBuffer))
            TransitionBuffer(tb->GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    // 3. scratch → UNORDERED_ACCESS
    D3D12_GPU_VIRTUAL_ADDRESS scratchAddr = 0;
    if (auto* sb = static_cast<D3D12Buffer*>(scratchBuffer)) {
        TransitionBuffer(sb->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        scratchAddr = sb->GetGpuAddress();
    }
    if (scratchAddr == 0) {
        HE_CORE_ERROR("BuildBLAS: 缺少 scratch 缓冲（构建被跳过）");
        return;
    }

    // 4. 构建
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
    bd.DestAccelerationStructureData    = as->GetDeviceAddress();
    bd.ScratchAccelerationStructureData = scratchAddr;
    bd.Inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bd.Inputs.Flags          = ToD3D12ASBuildFlags(desc.flags);
    bd.Inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bd.Inputs.NumDescs       = geoCount;
    bd.Inputs.pGeometryDescs = geos.data();
    if (update) {
        // 增量更新：读旧内容作为源（要求 AS 创建时带 ALLOW_UPDATE）
        bd.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        bd.SourceAccelerationStructureData = as->GetDeviceAddress();
    }

    list4->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);

    // 5. 加速结构写入完成：UAV 屏障（后续 TLAS 构建读它 / 光线遍历读它都要这条）
    D3D12_RESOURCE_BARRIER ub{};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = as->GetResource();
    m_List->ResourceBarrier(1, &ub);
}

void D3D12CommandList::BuildTLAS(IRHIAccelerationStructure* tlas, IRHIBuffer* scratchBuffer,
                                 IRHIBuffer* instanceBuffer, u32 instanceCount, bool update) {
    if (m_StreamMode && !StreamRTCarryEnabled()) {
        MarkStreamUnsupported("TLAS 构建");
        return;
    }
    if (m_StreamMode) {
        // 【P3】把 TLAS 构建编码进命令流（与 BuildBLAS 同一套思路；载荷结构 T10 时已存在，
        // Vulkan 侧早就在用，D3D12 此前只是没有录制分支）。
        if (!tlas || instanceCount == 0) return;
        auto* as = static_cast<D3D12AccelerationStructure*>(tlas);
        if (auto* ib = static_cast<D3D12Buffer*>(instanceBuffer))
            TransitionBuffer(ib->GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (!instanceBuffer || instanceBuffer->GetDeviceAddress() == 0) {
            HE_CORE_ERROR("BuildTLAS: 缺少实例缓冲（构建被跳过）");
            return;
        }
        if (auto* sb = static_cast<D3D12Buffer*>(scratchBuffer))
            TransitionBuffer(sb->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!scratchBuffer || scratchBuffer->GetDeviceAddress() == 0) {
            HE_CORE_ERROR("BuildTLAS: 缺少 scratch 缓冲（构建被跳过）");
            return;
        }
        Stream().Enqueue(RHICommandType::CmdBuildTLAS,
                         RHICommandBuildTLAS{ tlas, scratchBuffer, instanceBuffer,
                                              instanceCount, update ? 1u : 0u, 0u });
        D3D12_RESOURCE_BARRIER ub{};
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = as->GetResource();
        EmitBarrier(ub);
        static bool s_LoggedTLASStream = false;
        if (!s_LoggedTLASStream) {
            s_LoggedTLASStream = true;
            HE_CORE_INFO("[D3D12] 命令流承载光追：TLAS 构建已编码进流（{} 个实例）", instanceCount);
        }
        return;
    }
    if (!m_List || !tlas || instanceCount == 0) return;
    auto* as = static_cast<D3D12AccelerationStructure*>(tlas);
    ID3D12GraphicsCommandList4* list4 = GetList4();
    if (!list4) {
        HE_CORE_ERROR("BuildTLAS: 当前命令列表不支持 DXR（ID3D12GraphicsCommandList4）");
        return;
    }

    // 实例缓冲（引擎的 TLASInstanceDesc 与 D3D12_RAYTRACING_INSTANCE_DESC 逐字段一致，
    // 可直接当实例数组用）→ 非像素着色器可读
    D3D12_GPU_VIRTUAL_ADDRESS instanceAddr = 0;
    if (auto* ib = static_cast<D3D12Buffer*>(instanceBuffer)) {
        TransitionBuffer(ib->GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        instanceAddr = ib->GetGpuAddress();
    }
    if (instanceAddr == 0) {
        HE_CORE_ERROR("BuildTLAS: 缺少实例缓冲（构建被跳过）");
        return;
    }
    D3D12_GPU_VIRTUAL_ADDRESS scratchAddr = 0;
    if (auto* sb = static_cast<D3D12Buffer*>(scratchBuffer)) {
        TransitionBuffer(sb->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        scratchAddr = sb->GetGpuAddress();
    }
    if (scratchAddr == 0) {
        HE_CORE_ERROR("BuildTLAS: 缺少 scratch 缓冲（构建被跳过）");
        return;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
    bd.DestAccelerationStructureData    = as->GetDeviceAddress();
    bd.ScratchAccelerationStructureData = scratchAddr;
    bd.Inputs.Type        = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    // 【P3 诊断】HE_D3D12_STREAM_RT_DBG=1：确认回放侧执行了原生 TLAS 构建（实例数 / 实例地址）
    {
        static const bool s_dbg = (std::getenv("HE_D3D12_STREAM_RT_DBG") != nullptr);
        static u32 s_n = 0;
        ++s_n;
        if (s_dbg && (s_n <= 8u || (s_n % 120u) == 0u)) {
            HE_CORE_INFO("[RTDBG] 原生 BuildTLAS 执行：replay={} 第{}次 实例={} 实例地址=0x{:x} AS=0x{:x}",
                         m_StreamReplay ? 1 : 0, s_n, instanceCount, instanceAddr,
                         as->GetDeviceAddress());
        }
    }
    bd.Inputs.Flags       = ToD3D12ASBuildFlags(ASBuildFlags::PreferFastTrace);
    bd.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bd.Inputs.NumDescs    = instanceCount;
    bd.Inputs.InstanceDescs = instanceAddr;
    if (update) {
        bd.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        bd.SourceAccelerationStructureData = as->GetDeviceAddress();
    }

    list4->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);

    D3D12_RESOURCE_BARRIER ub{};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = as->GetResource();
    m_List->ResourceBarrier(1, &ub);
}

void D3D12CommandList::BindRTPipeline(IRHIRayTracingPipelineState* rtPSO) {
    if (m_StreamMode && !StreamRTCarryEnabled()) {
        MarkStreamUnsupported("光追管线绑定");
        return;
    }
    if (m_StreamMode) {
        // 【P3】RT 管线绑定编码进流：SetPipelineState1 + SetComputeRootSignature 由回放侧下发。
        // 录制期这里只更新本列表的绑定状态（后续 TraceRays 的资源状态解析要按库反射查 m_RtPipeline），
        // 并把"设了根签名 ⇒ 根参数全失效"这个副作用一并记下（回放列表会重走同一条原生路径）。
        if (!rtPSO) return;
        auto* rt = static_cast<D3D12RayTracingPipelineState*>(rtPSO);
        if (!rt->GetStateObject()) return;
        m_Pipeline          = nullptr;
        m_RtPipeline        = rt;
        m_IsComputePipeline = true;
        m_RootArgsDirty     = true;
        Stream().Enqueue(RHICommandType::CmdBindRTPipeline, RHICommandBindRTPipeline{ rtPSO });
        return;
    }
    if (!m_List || !rtPSO) return;
    auto* rt = static_cast<D3D12RayTracingPipelineState*>(rtPSO);
    ID3D12GraphicsCommandList4* list4 = GetList4();
    if (!list4 || !rt->GetStateObject()) return;

    m_Pipeline   = nullptr;   // 光追没有普通 PSO：资源状态走 RT 状态对象的库反射
    m_RtPipeline = rt;
    // 根参数按「计算管线」的口径下发（SetComputeRoot*），与 Vulkan 的绑定路径一致
    m_IsComputePipeline = true;
    if (!m_Recording) { m_RootArgsDirty = true; return; }   // 录制开始前只记状态（同 SetPipeline）

    list4->SetPipelineState1(rt->GetStateObject());
    m_List->SetComputeRootSignature(rt->GetGlobalRootSignature());
    m_RootSigBound = true;    // 光追的全局根签名已绑：之后才允许下发根参数
    m_RootArgsDirty = true;   // 设根签名会清空全部根参数，下一次派发前要重新下发
}

void D3D12CommandList::TraceRays(const SBTDesc& sbt, u32 width, u32 height, u32 depth) {
    if (m_StreamMode && !StreamRTCarryEnabled()) {
        MarkStreamUnsupported("光线派发");
        return;
    }
    if (m_StreamMode) {
        // 【P3】光线派发编码进流：与 Dispatch 同一套录制期解析（补资源状态 + 拍描述符表快照），
        // 再把 SBT 四个槽位与派发尺寸写进流 arena（SBTDesc 的 u64 字段合计超过 64B 内联上限）。
        if (!m_RtPipeline || !sbt.buffer) return;
        StreamDrawPrologue();
        RHICommandTraceRays p{};
        p.buffer         = sbt.buffer;
        p.rayGenOffset   = sbt.rayGen.handleOffset;   p.rayGenStride   = sbt.rayGen.stride;
        p.missOffset     = sbt.miss.handleOffset;     p.missStride     = sbt.miss.stride;
        p.hitOffset      = sbt.hit.handleOffset;      p.hitStride      = sbt.hit.stride;
        p.callableOffset = sbt.callable.handleOffset; p.callableStride = sbt.callable.stride;
        p.width  = width;
        p.height = height;
        p.depth  = depth;
        Stream().EnqueueArena(RHICommandType::CmdTraceRays, p);
        StreamDrawEpilogue();
        static bool s_LoggedTraceStream = false;
        if (!s_LoggedTraceStream) {
            s_LoggedTraceStream = true;
            HE_CORE_INFO("[D3D12] 命令流承载光追：DispatchRays 已编码进流（{}x{}x{}）", width, height, depth);
        }
        if (std::getenv("HE_D3D12_STREAM_RT_DBG")) {
            static u32 s_dbgN = 0;
            ++s_dbgN;
            if (s_dbgN <= 8u || (s_dbgN % 120u) == 0u) {
                HE_CORE_INFO("[RTDBG] 录制期编码 DispatchRays：第{}次 rtPSO={} sbtBuf={} rg(off={},stride={}) "
                             "miss(off={},stride={}) hit(off={},stride={})",
                             s_dbgN, (void*)m_RtPipeline, (void*)sbt.buffer, sbt.rayGen.handleOffset,
                             sbt.rayGen.stride, sbt.miss.handleOffset, sbt.miss.stride,
                             sbt.hit.handleOffset, sbt.hit.stride);
            }
        }
        return;
    }
    if (!m_List || !m_RtPipeline || !sbt.buffer) {
        // 【P3 诊断】HE_D3D12_STREAM_RT_DBG=1 时打印本次派发被跳过的原因
        if (std::getenv("HE_D3D12_STREAM_RT_DBG")) {
            HE_CORE_WARN("[RTDBG] TraceRays 跳过：list={} rtPSO={} sbtBuf={} replay={}",
                         (void*)m_List.Get(), (void*)m_RtPipeline, (void*)sbt.buffer, m_StreamReplay ? 1 : 0);
        }
        return;
    }
    ID3D12GraphicsCommandList4* list4 = GetList4();
    {
        static const bool s_dbg = (std::getenv("HE_D3D12_STREAM_RT_DBG") != nullptr);
        static u32 s_n = 0;
        if (s_dbg && ++s_n <= 8u) {
            HE_CORE_INFO("[RTDBG] 原生 TraceRays 执行：replay={} {}x{}x{} sbtBuf={} list4={}",
                         m_StreamReplay ? 1 : 0, width, height, depth, (void*)sbt.buffer, (void*)list4);
        }
    }
    if (!list4) return;

    // 与 Dispatch 一致：先按反射补资源状态、再下发根参数（描述符表 / 推送常量）
    EnsureBoundResourceStates();
    FlushRootArgs();

    // SBT 布局由 RTPass::BuildSBT 决定：RayGen → Miss → Hit → Callable 依次相接，
    // 每段大小取「下一段起点 − 本段起点」，最后一段取「缓冲大小 − 起点」（手册 T5.7）。
    const u64 va      = sbt.buffer->GetDeviceAddress();
    const u64 bufSize = static_cast<u64>(sbt.buffer->GetSize());
    D3D12_DISPATCH_RAYS_DESC d{};
    d.RayGenerationShaderRecord = { va + sbt.rayGen.handleOffset,
                                    sbt.rayGen.stride ? sbt.rayGen.stride : 0 };
    if (sbt.miss.stride && sbt.hit.handleOffset >= sbt.miss.handleOffset) {
        d.MissShaderTable.StartAddress  = va + sbt.miss.handleOffset;
        d.MissShaderTable.SizeInBytes   = sbt.hit.handleOffset - sbt.miss.handleOffset;
        d.MissShaderTable.StrideInBytes = sbt.miss.stride;
    }
    if (sbt.hit.stride && sbt.callable.handleOffset >= sbt.hit.handleOffset) {
        d.HitGroupTable.StartAddress  = va + sbt.hit.handleOffset;
        d.HitGroupTable.SizeInBytes   = sbt.callable.handleOffset - sbt.hit.handleOffset;
        d.HitGroupTable.StrideInBytes = sbt.hit.stride;
    }
    if (sbt.callable.stride && sbt.callable.handleOffset < bufSize) {
        d.CallableShaderTable.StartAddress  = va + sbt.callable.handleOffset;
        d.CallableShaderTable.SizeInBytes   = bufSize - sbt.callable.handleOffset;
        d.CallableShaderTable.StrideInBytes = sbt.callable.stride;
    }
    d.Width  = width;
    d.Height = height;
    d.Depth  = depth ? depth : 1;

    // 【P3 诊断】派发当时实际绑定的表 / 根参数 / 堆状态（直接录制与回放逐项对比）
    if (StreamRTDebugEnabled()) {
        static u32 s_dbgTrAll = 0;
        ++s_dbgTrAll;
        if (s_dbgTrAll <= 8u || (s_dbgTrAll % 120u) == 0u) {
            HE_CORE_INFO("[RTDBG] DispatchRays 根参数：replay={} 第{}次 srv0Slot={} uav0Slot={} "
                         "pushGpu=0x{:x} heapsSet={} rtPSO={}",
                         m_StreamReplay ? 1 : 0, s_dbgTrAll, DbgDescSlot(m_Device, m_RootTables[8]),
                         DbgDescSlot(m_Device, m_RootTables[16]), m_PushGpu,
                         m_HeapsSet ? 1 : 0, static_cast<void*>(m_RtPipeline));
        }
    }

    list4->DispatchRays(&d);
    AfterDrawOrDispatch();
    m_PushDirty = true;   // 常量槽已被本次发射消费（同 Dispatch）
}

// 光追回退路径：库反射不可用时，按描述符集里记录的描述符类型推断「读 / 写」并补状态转换。
// 只处理 DEFAULT / CUSTOM 堆上的资源（UPLOAD / READBACK 堆的缓冲不能转状态）。
void D3D12CommandList::EnsureRTSetStatesByType() {
    if (!m_List || !m_Device) return;
    const D3D12_RESOURCE_STATES kRead = ShaderReadState();
    std::vector<ID3D12Resource*> handled;   // 同一资源在多个 set 里绑定时只转一次

    auto request = [&](ID3D12Resource* res, bool isBuffer, bool uav) {
        if (!res) return;
        if (std::find(handled.begin(), handled.end(), res) != handled.end()) return;
        D3D12_HEAP_PROPERTIES hp{};
        if (FAILED(res->GetHeapProperties(&hp, nullptr)) ||
            (hp.Type != D3D12_HEAP_TYPE_DEFAULT && hp.Type != D3D12_HEAP_TYPE_CUSTOM)) return;
        // 正作为本 pass 附件的资源不能同时转成可读
        if (std::find(m_PassAttachments.begin(), m_PassAttachments.end(), res) != m_PassAttachments.end()) return;
        handled.push_back(res);

        const D3D12_RESOURCE_STATES cur  = isBuffer ? GetBufferState(res) : UsableTextureState(res);
        const D3D12_RESOURCE_STATES want = uav ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : kRead;
        if (cur == want || (!uav && (cur & kRead) == kRead)) return;
        // 缓冲首次使用由 COMMON 隐式提升（与 EnsureBoundResourceStates 同一口径）
        if (isBuffer && cur == D3D12_RESOURCE_STATE_COMMON) { m_BufferStates[res] = want; return; }
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = cur;
        b.Transition.StateAfter  = want;
        m_BarrierScratch.push_back(b);
        if (isBuffer) m_BufferStates[res] = want;
        else          m_Device->SetResourceState(res, want);
    };

    for (u32 i = 0; i < 8u; ++i) {
        const DescriptorSetHandle set = m_BoundSets[i];
        if (set == kInvalidSet) continue;
        m_Device->ForEachSetBinding(set, [&](ID3D12Resource* res, bool isBuffer, bool uav, u32) {
            request(res, isBuffer, uav);
        });
    }
}

void D3D12CommandList::PipelineBarrier(PipelineStage srcStage, PipelineStage dstStage,
                                       ResourceState srcState, ResourceState dstState) {
    if (m_StreamMode) { Stream().Enqueue(RHICommandType::CmdPipelineBarrier, RHICommandPipelineBarrier{ static_cast<u32>(srcStage),
            static_cast<u32>(dstStage), static_cast<u32>(srcState), static_cast<u32>(dstState) });
        return;
    }
    // 无资源参数的全局屏障：D3D12 要求屏障绑定到具体资源，
    // 引擎里这种调用通常用于「pass 之间的全局同步」，D3D12 下由 UAV 屏障近似。
    if (!m_List || m_IsBundle) return;   // bundle 不能发屏障
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.UAV.pResource = nullptr;   // nullptr = 全局 UAV 屏障（所有 UAV）
    m_List->ResourceBarrier(1, &b);
}

void D3D12CommandList::PipelineBarrier(PipelineStage srcStage, PipelineStage dstStage,
                                       ResourceState srcState, ResourceState dstState,
                                       IRHITexture* texture) {
    // 【T10】回放侧不再解析：录制期已按设备状态表解析并编码进命令流（CmdResolvedBarrier）
    if (m_StreamReplay) return;
    if (!texture || m_IsBundle) return;   // bundle 不能发屏障
    auto* t = static_cast<D3D12Texture*>(texture);
    ActivateAliasedTexture(t->GetResource());   // 瞬态纹理首次使用：先激活
    D3D12_RESOURCE_STATES from = ToD3D12ResourceState(srcState, t->IsDepth());
    D3D12_RESOURCE_STATES to   = ToD3D12ResourceState(dstState, t->IsDepth());
    // 计算列表：目标状态去掉图形专用位；去不掉（转成渲染目标 / 深度）说明这次转换属于图形队列，跳过
    if (!FilterStateForQueue(to)) return;
    FilterStateForQueue(from);
    // 【UAV → UAV 必须是 UAV 屏障】引擎在相互依赖的计算派发之间发「UAV → UAV」屏障（如 SDF 的清空 → 散射 →
    // 逐轮洪泛），Vulkan 下它是内存屏障。此前状态相同就直接返回、什么都不做：前后派发在 GPU 上可以重叠执行，
    // 后一次读到前一次未写完的数据（实测 Lumen 逐 mesh 距离场在 D3D12 下大片未写入 / 误差偏大）
    auto uavBarrier = [&]() {
        D3D12_RESOURCE_BARRIER ub{};
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = t->GetResource();
        EmitBarrier(ub);   // 流模式编码进命令流；普通列表直接下发
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
            m_Device ? UsableTextureState(bres) : from;   // 计算列表：图形专用状态先交接
        if (actualFrom == to) {
            if (to == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) uavBarrier();   // 已是 UAV：仍要同步前后派发
            return;
        }
        b.Transition.StateBefore = actualFrom;
    b.Transition.StateAfter  = to;
    EmitBarrier(b);
        if (m_Device) m_Device->SetResourceState(bres, to);
}

void D3D12CommandList::CopyBuffer(IRHIBuffer* src, IRHIBuffer* dst, u64 size,
                                  u64 srcOffset, u64 dstOffset) {
    if (!src || !dst) return;
    if (m_StreamReplay) {   // 【T10】回放：缓冲状态转换已由录制期解析进命令流，只做拷贝
        if (m_List) m_List->CopyBufferRegion(static_cast<D3D12Buffer*>(dst)->GetResource(), dstOffset,
                                             static_cast<D3D12Buffer*>(src)->GetResource(), srcOffset, size);
        return;
    }
    if (!m_List && !m_StreamMode) return;
    auto* s = static_cast<D3D12Buffer*>(src);
    auto* d = static_cast<D3D12Buffer*>(dst);
    // 拷贝要求源 / 目标分别处于 COPY_SOURCE / COPY_DEST：同一列表里缓冲可能已被提升为 UAV 等状态，
    // 这时必须显式转换（只处理 DEFAULT/CUSTOM 堆；UPLOAD/READBACK 堆的缓冲状态固定）
    TransitionBuffer(s->GetResource(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionBuffer(d->GetResource(), D3D12_RESOURCE_STATE_COPY_DEST);
    // 【T10】流模式：上面的转换已经编码进命令流，这里只把"拷贝"这一条命令入流
    if (m_StreamMode) {
        Stream().Enqueue(RHICommandType::CmdCopyBuffer,
                         RHICommandCopyBuffer{ src, dst, size, srcOffset, dstOffset });
        return;
    }
    m_List->CopyBufferRegion(d->GetResource(), dstOffset, s->GetResource(), srcOffset, size);
}

// ---------------------------------------------------------------------------
//  异步计算列表的资源状态
//
//  COMPUTE 类型的列表上，屏障的前后状态都不能含图形专用位（PIXEL_SHADER_RESOURCE /
//  RENDER_TARGET / DEPTH_* 等），否则调试层报错。设备级的纹理状态跟踪不区分队列，
//  纹理常停在图形状态（如上一帧被当作 PIXEL | NON_PIXEL 采样、或是渲染目标）。
//  处理方式：
//    · 目标状态：去掉 PIXEL_SHADER_RESOURCE 位，只读状态在计算列表上即 NON_PIXEL_SHADER_RESOURCE；
//    · 起始状态：若含图形专用位，记一条「交接转换」（图形专用状态 → 去掉图形位后的状态，
//      全去光则为 COMMON），由设备在提交本列表前放到图形队列上执行，计算队列等它完成后再执行本列表
//      （见 D3D12Device::Submit）。交接后设备登记的状态随之更新，同一资源在本次录制中只交接一次。
//  前提：交接转换在提交时才执行，假定异步计算 pass 用到的资源不会被「已录制、尚未提交」的
//  图形命令改变状态（RenderGraph 只把只读上一帧数据的计算 pass 放到异步队列，满足这一点）。
// ---------------------------------------------------------------------------
namespace {
/// 计算列表上不允许出现的图形专用状态位
constexpr D3D12_RESOURCE_STATES kGraphicsOnlyStates =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_RENDER_TARGET |
    D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ |
    D3D12_RESOURCE_STATE_STREAM_OUT | D3D12_RESOURCE_STATE_RESOLVE_DEST |
    D3D12_RESOURCE_STATE_RESOLVE_SOURCE | D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
} // namespace

D3D12_RESOURCE_STATES D3D12CommandList::ShaderReadState() const {
    return IsComputeQueueList()
        ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
        : (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

bool D3D12CommandList::FilterStateForQueue(D3D12_RESOURCE_STATES& state) const {
    if (!IsComputeQueueList()) return true;
    const D3D12_RESOURCE_STATES original = state;
    state &= ~D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    // 只剩 PIXEL 位的只读请求在计算列表上等价于 NON_PIXEL 读
    if (state == D3D12_RESOURCE_STATE_COMMON && original != D3D12_RESOURCE_STATE_COMMON)
        state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if ((state & kGraphicsOnlyStates) != 0) {
        static bool s_Warned = false;   // 只提示一次，避免每帧刷屏
        if (!s_Warned) {
            s_Warned = true;
            HE_CORE_WARN("D3D12CommandList: 计算列表上请求转到图形专用状态 0x{:x}，已跳过（应在图形队列上转换）",
                         static_cast<u32>(original));
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  瞬态纹理的激活（内存别名）
//  瞬态纹理与生命周期不重叠的其他瞬态纹理共用内存。在本纹理首次被使用前：
//    1. 别名屏障，Before 为空（= 该段内存上之前的任何资源），After 为本纹理 ⇒ 之前资源的写入
//       完成后才轮到本纹理，GPU 缓存也随之失效；
//    2. 渲染目标 / 深度：转到 RENDER_TARGET / DEPTH_WRITE 后 DiscardResource，初始化压缩元数据
//      （复用内存上的这两类资源内容未定义，D3D12 要求先 Clear / Discard / 整体拷贝再使用）。
//  Discard 只能在图形列表上做：计算列表上只发别名屏障（渲染目标 / 深度本就不会先在计算列表上用）。
// ---------------------------------------------------------------------------
void D3D12CommandList::ActivateAliasedTexture(ID3D12Resource* res) {
    if (!m_Device || !res || m_IsBundle) return;   // bundle 不能发屏障
    D3D12_RESOURCE_STATES discardState = D3D12_RESOURCE_STATE_COMMON;
    if (!m_Device->TakeAliasActivation(res, discardState)) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    b.Aliasing.pResourceBefore = nullptr;
    b.Aliasing.pResourceAfter  = res;
    EmitBarrier(b);
    if (discardState == D3D12_RESOURCE_STATE_COMMON || IsComputeQueueList()) return;
    const D3D12_RESOURCE_STATES cur = m_Device->GetResourceState(res);
    if (cur != discardState) {
        D3D12_RESOURCE_BARRIER t{};
        t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        t.Transition.pResource   = res;
        t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        t.Transition.StateBefore = cur;
        t.Transition.StateAfter  = discardState;
        EmitBarrier(t);
        m_Device->SetResourceState(res, discardState);
    }
    EmitDiscard(res);
}

// ---------------------------------------------------------------------------
//  SyncBackBufferStateForAttachment —— 【T9】离屏 pass 用后缓冲做附件时的状态同步
//
//  06.GILab 的最终合成（以及其他"直接写后缓冲"的离屏 pass）会把交换链后缓冲当作普通
//  颜色附件传进来。后缓冲的 RTV 现在登记在设备的资源状态表里（见
//  D3D12SwapChain::CreateBuffers），BeginOffscreenPass / BeginOffscreenPassMRT 的通用
//  路径会正确地做 PRESENT→RENDER_TARGET 转换并更新设备表；但交换链自己那份
//  m_BackBufferState 只有 BeginRenderPass 会写。两份记录一旦分叉，截图/呈现就会声明
//  错误的 StateBefore（调试层 id=527，约 6 次运行出现 1 次）。
//  这里在附件确实是后缓冲时把镜像同步过去，保证两份记录永远一致。
// ---------------------------------------------------------------------------
void D3D12CommandList::SyncBackBufferStateForAttachment(ID3D12Resource* res) {
    if (!res) return;
    if (!m_SwapChain && m_Device) m_SwapChain = m_Device->GetSwapChainPtr();
    if (!m_SwapChain) return;
    auto* sc = static_cast<D3D12SwapChain*>(m_SwapChain);
    if (sc->IsBackBuffer(res)) sc->MarkBackBufferRendered();
}

D3D12_RESOURCE_STATES D3D12CommandList::UsableTextureState(ID3D12Resource* res) {
    if (!m_Device || !res) return D3D12_RESOURCE_STATE_COMMON;
    ActivateAliasedTexture(res);   // 瞬态纹理首次使用：先激活（所有取纹理屏障起始状态的路径都经过这里）
    const D3D12_RESOURCE_STATES cur = m_Device->GetResourceState(res);
    if (!IsComputeQueueList() || (cur & kGraphicsOnlyStates) == 0) return cur;
    // 去掉图形专用位（PIXEL | NON_PIXEL → NON_PIXEL；渲染目标 / 深度 → COMMON）
    D3D12_RESOURCE_STATES usable = cur & ~kGraphicsOnlyStates;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = cur;
    b.Transition.StateAfter  = usable;
    m_QueueHandoffs.push_back(b);
    m_Device->SetResourceState(res, usable);
    return usable;
}

void D3D12CommandList::TransitionBuffer(ID3D12Resource* res, D3D12_RESOURCE_STATES want) {
    if (!res || !m_Device) return;
    // 【T10】回放侧不再解析：录制期已把缓冲的状态转换编码进命令流
    if (m_StreamReplay) return;
    if (m_IsBundle) { m_BundleBufferTransitions.emplace_back(res, want); return; }   // 由主列表补发
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
    EmitBarrier(b);               // 流模式编码进命令流，普通列表直接下发
    m_BufferStates[res] = want;   // 本列表内的缓冲状态（列表执行完后衰减回 COMMON，见 Begin）
}

void D3D12CommandList::CopyTextureToTexture(IRHITexture* src, IRHITexture* dst) {
    if (!src || !dst) return;
    auto* s = static_cast<D3D12Texture*>(src);
    auto* d = static_cast<D3D12Texture*>(dst);
    ID3D12Resource* sr = s->GetResource();
    ID3D12Resource* dr = d->GetResource();
    if (!sr || !dr || sr == dr || !m_Device) return;
    // 【T10】回放侧：临时转换已由录制期解析进命令流，这里只做拷贝
    if (m_StreamReplay) {
        if (m_List) m_List->CopyResource(dr, sr);
        return;
    }
    if (!m_List && !m_StreamMode) return;
    // CopyResource 要求源 / 目标分别处于 COPY_SOURCE / COPY_DEST：临时转过去，拷完转回原状态
    //（设备级状态记录不变）。此前不做转换，渲染目标等状态下直接拷贝属非法调用
    // 计算列表：图形专用状态先交接到计算可用状态（拷完转回的也是交接后的状态）
    const D3D12_RESOURCE_STATES ss = UsableTextureState(sr);
    const D3D12_RESOURCE_STATES ds = UsableTextureState(dr);
    auto add = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES fromState, D3D12_RESOURCE_STATES toState) {
        if (fromState == toState) return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = fromState;
        b.Transition.StateAfter  = toState;
        EmitBarrier(b);
    };
    add(sr, ss, D3D12_RESOURCE_STATE_COPY_SOURCE);
    add(dr, ds, D3D12_RESOURCE_STATE_COPY_DEST);
    // 【T10】流模式：上面的转换已入流，这里把"拷贝"这一条命令入流（回放侧只做 CopyResource）
    if (m_StreamMode) {
        Stream().Enqueue(RHICommandType::CmdCopyTextureToTexture, RHICommandCopyTextureToTexture{ src, dst });
    } else {
        m_List->CopyResource(dr, sr);
    }
    add(sr, D3D12_RESOURCE_STATE_COPY_SOURCE, ss);
    add(dr, D3D12_RESOURCE_STATE_COPY_DEST, ds);
}

// ClearDepthStencil —— 清除深度/模板纹理（对应 Vulkan 的 vkCmdClearDepthStencilImage）
// D3D12 只能通过 DSV 清深度：先按设备登记的状态把资源转到 DEPTH_WRITE，再 ClearDepthStencilView。
// 清除后资源留在 DEPTH_WRITE，正好供随后的深度测试 pass 使用（与 Vulkan 版末尾转到附件布局一致）。
void D3D12CommandList::ClearDepthStencil(IRHITexture* texture, float depth) {
    if (!texture || m_IsBundle) return;   // bundle 不能发屏障与清除
    if (IsComputeQueueList()) {   // DEPTH_WRITE 与 ClearDepthStencilView 只能在图形列表上使用
        HE_CORE_WARN("D3D12CommandList::ClearDepthStencil: 计算列表上不能清除深度，已忽略");
        return;
    }
    auto* t = static_cast<D3D12Texture*>(texture);
    ID3D12Resource* res = t->GetResource();
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = t->GetDsv();
    if (!res || !dsv.ptr) return;   // 没有 DSV 的纹理无法清深度
    // 带模板的格式同时清模板（与 Vulkan 版一致清成 0）
    const Format fmt = t->GetFormat();
    D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAG_DEPTH;
    if (fmt == Format::D24_UNORM_S8_UINT || fmt == Format::D32_FLOAT_S8_UINT)
        flags |= D3D12_CLEAR_FLAG_STENCIL;
    if (m_StreamMode) {
        // 【T10】录制期解析：转 DEPTH_WRITE + 清除命令一起入流
        const D3D12_RESOURCE_STATES from = m_Device ? m_Device->GetResourceState(res)
                                                     : D3D12_RESOURCE_STATE_COMMON;
        ActivateAliasedTexture(res);
        if (from != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource   = res;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = from;
            b.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            EmitBarrier(b);
            if (m_Device) m_Device->SetResourceState(res, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
        Stream().Enqueue(RHICommandType::CmdClearDepthStencil,
                         RHICommandClearDepthStencil{ texture, depth, 0u });
        return;
    }
    if (!m_List) return;
    // 【T10】回放侧：状态转换已随命令流完成，只做清除
    if (m_StreamReplay) {
        m_List->ClearDepthStencilView(dsv, flags, depth, 0, 0, nullptr);
        return;
    }
    ActivateAliasedTexture(res);    // 瞬态纹理首次使用：先激活

    // 以设备登记的真实状态作屏障的 StateBefore（同 PipelineBarrier）
    const D3D12_RESOURCE_STATES from =
        m_Device ? m_Device->GetResourceState(res) : D3D12_RESOURCE_STATE_COMMON;
    if (from != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        EmitBarrier(b);
        if (m_Device) m_Device->SetResourceState(res, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }

    m_List->ClearDepthStencilView(dsv, flags, depth, 0, 0, nullptr);
}

void D3D12CommandList::CopyTextureToBuffer(IRHITexture* src, IRHIBuffer* dst, u32 x, u32 y,
                                           u32 width, u32 height, u64 bufferOffset) {
    if (!m_Device || !m_Device->GetDevice() || !src || !dst) return;
    auto* t = static_cast<D3D12Texture*>(src);
    auto* b = static_cast<D3D12Buffer*>(dst);
    ID3D12Resource* texRes = t->GetResource();
    ID3D12Resource* bufRes = b->GetResource();
    if (!texRes || !bufRes) return;
    if (m_StreamMode) {
        // 【T10】录制期解析：纹理临时转 COPY_SOURCE、目标缓冲转 COPY_DEST（回放侧不再解析）；
        // 中转缓冲与逐行拷贝仍由回放侧完成（那是纯资源操作，不依赖引擎状态）。
        ActivateAliasedTexture(texRes);
        const D3D12_RESOURCE_STATES texState = UsableTextureState(texRes);
        if (texState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            D3D12_RESOURCE_BARRIER tb{};
            tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            tb.Transition.pResource   = texRes;
            tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            tb.Transition.StateBefore = texState;
            tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
            EmitBarrier(tb);
        }
        TransitionBuffer(bufRes, D3D12_RESOURCE_STATE_COPY_DEST);
        Stream().Enqueue(RHICommandType::CmdCopyTextureToBuffer,
                         RHICommandCopyTextureToBuffer{ src, dst, x, y, width, height, 0u, bufferOffset });
        // 拷完把纹理转回原状态（流里排在拷贝命令之后）
        if (texState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            D3D12_RESOURCE_BARRIER tb{};
            tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            tb.Transition.pResource   = texRes;
            tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            tb.Transition.StateAfter  = texState;
            EmitBarrier(tb);
        }
        return;
    }
    if (!m_List) return;
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
    // 【T10】回放侧跳过这两处共享资源转换：录制期已把它们编码进命令流（CmdResolvedBarrier）
    const bool replay = m_StreamReplay;
    const D3D12_RESOURCE_STATES texState =
        replay ? D3D12_RESOURCE_STATE_COMMON : UsableTextureState(texRes);   // 计算列表：图形专用状态先交接
    D3D12_RESOURCE_BARRIER tb{};
    tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    tb.Transition.pResource   = texRes;
    tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    tb.Transition.StateBefore = texState;
    tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (!replay && texState != D3D12_RESOURCE_STATE_COPY_SOURCE) m_List->ResourceBarrier(1, &tb);

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

    if (!replay && texState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
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
    if (!replay) TransitionBuffer(bufRes, D3D12_RESOURCE_STATE_COPY_DEST);   // 回放：已随命令流解析
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
    if (m_StreamMode) { Stream().Enqueue(RHICommandType::CmdTimelineSignal, RHICommandTimeline{ static_cast<u64>(fence), value }); return; }
    m_PendingSignals.emplace_back(fence, value);
}

void D3D12CommandList::SetTimelineWait(RHIFenceHandle fence, u64 value) {
    if (m_StreamMode) { Stream().Enqueue(RHICommandType::CmdTimelineWait, RHICommandTimeline{ static_cast<u64>(fence), value }); return; }
    m_PendingWaits.emplace_back(fence, value);
}

// 列表自己的 Submit 是「就地提交」（与 Vulkan 的 VulkanCommandList::Submit 一致，不走提交路由）：
// RenderGraph 的异步计算列表提交后随即析构，不能交给 RHI 线程稍后执行
void D3D12CommandList::Submit() {
    if (TraceEnabled()) { std::printf("[TRACE] Submit\n"); std::fflush(stdout); }
    if (!m_Device) return;
    if (m_StreamMode) {
        // 命令流与"录制期用过的描述符快照块掩码"一起交出（回放提交后据此登记围栏）
        RHICommandList* s = GetRecordedStream();
        if (s) s->SetAuxMask(m_SetRingChunkMask);
        // 影子列表在录制线程上先建出来（设备登记表非线程安全，见 D3D12Device::Submit）
        (void)GetReplayList();
        m_Device->ExecuteSubmit(this, s);
        return;
    }
    PackSubmission();
    m_Device->SubmitNow(this);
}

// ---------------------------------------------------------------------------
//  命令流模式
// ---------------------------------------------------------------------------
void D3D12CommandList::EnableCommandStream() {
    // 只有图形队列的主列表支持（bundle 与计算列表永远真实录制）
    const char* e = std::getenv("HE_RHI_COMMAND_STREAM");
    const bool want = (e && e[0] == '1') && !m_IsBundle && m_QueueType == QueueType::Graphics;
    // 【HE_RHI_COMMAND_STREAM_RT=0：显式回退】这个开关表示"命令流不承载光追"。
    // 不能只在命中光追命令时跳过该条命令：BLAS 构建被跳过后 RTPass 仍会把几何哈希记为
    // "已构建"，之后（直接录制）再也不会重建 BLAS ⇒ TLAS 的实例指向从未构建的 BLAS，
    // 画面永远只剩天空。因此关闭承载光追时**整帧直接录制**，第一帧起就是正确画面；
    // 同时跳过光追命令还会在回放侧留下"没有根签名就设根参数"的非法序列（D3D12 会静默卡死）。
    if (want && !StreamRTCarryEnabled()) {
        m_StreamMode = false;
        static bool s_WarnedRT = false;
        if (!s_WarnedRT) {
            s_WarnedRT = true;
            HE_CORE_WARN("[D3D12] HE_RHI_COMMAND_STREAM_RT=0：命令流不承载光追，"
                         "帧主列表改为直接录制（画面与未开命令流一致）");
        }
        return;
    }
    // 【T10】回退：录制中遇到过命令流无法承载的调用（光追构建 / 派发等）⇒ 从本帧起直接录制。
    // 打印一次明确原因，绝不静默丢命令。
    if (want && m_StreamFallback) {
        m_StreamMode = false;
        static bool s_Warned = false;
        if (!s_Warned) {
            s_Warned = true;
            HE_CORE_WARN("[D3D12] 命令流模式已回退为直接录制：录制期出现命令流无法承载的调用"
                         "（原因见上一条 [D3D12] 警告）");
        }
        return;
    }
    m_StreamMode = want;
    if (m_StreamMode && !m_Streams[m_StreamSlot]) m_Streams[m_StreamSlot] = std::make_unique<RHICommandList>();
}

void D3D12CommandList::MarkStreamUnsupported(const char* what) {
    if (m_StreamFallback) return;
    m_StreamFallback = true;
    HE_CORE_WARN("[D3D12] 命令流无法承载 {}：本帧继续按命令流提交（该调用被跳过），"
                 "从下一帧起帧主列表回退为直接录制", what ? what : "该调用");
}

D3D12CommandList* D3D12CommandList::GetReplayList() {
    if (!m_ReplayList && m_Device) {
        auto r = std::make_unique<D3D12CommandList>();
        if (r->Initialize(m_Device, m_QueueType)) m_ReplayList = std::move(r);
    }
    return m_ReplayList.get();
}

// ---------------------------------------------------------------------------
//  【T10】命令流回放入口：只按录制期解析好的数据下原生指令，不查任何引擎侧可变状态
// ---------------------------------------------------------------------------
void D3D12CommandList::ApplyResolvedBarrier(void* resource, ResolvedBarrierType type,
                                            u32 subresource, u32 before, u32 after) {
    if (!m_List) return;
    auto* res = static_cast<ID3D12Resource*>(resource);
    switch (type) {
    case ResolvedBarrierType::Transition: {
        if (!res) return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.Subresource = subresource;
        b.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(before);
        b.Transition.StateAfter  = static_cast<D3D12_RESOURCE_STATES>(after);
        m_List->ResourceBarrier(1, &b);
        return;
    }
    case ResolvedBarrierType::Uav: {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = res;   // 空表示全局 UAV 屏障
        m_List->ResourceBarrier(1, &b);
        return;
    }
    case ResolvedBarrierType::Aliasing: {
        if (!res) return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
        b.Aliasing.pResourceBefore = nullptr;
        b.Aliasing.pResourceAfter  = res;
        m_List->ResourceBarrier(1, &b);
        return;
    }
    case ResolvedBarrierType::Discard:
        if (res) m_List->DiscardResource(res, nullptr);
        return;
    }
}

void D3D12CommandList::ApplyResolvedDescriptorTable(u32 setIndex, u64 set, u64 srv,
                                                    u64 uav, u64 sampler) {
    if (setIndex >= 8u) return;
    // 【P3 诊断】回放侧收到的描述符表与录制期编码的是同一组句柄（此处换算成堆内槽位）
    if (StreamRTDebugEnabled()) {
        static u32 s_dbgApply = 0;
        if (++s_dbgApply <= 16u || (s_dbgApply % 120u) == 0u) {
            D3D12_GPU_DESCRIPTOR_HANDLE s{}; s.ptr = srv;
            D3D12_GPU_DESCRIPTOR_HANDLE u{}; u.ptr = uav;
            HE_CORE_INFO("[RTDBG] 回放描述符表：setIndex={} set={} srvSlot={} uavSlot={}",
                         setIndex, set, DbgDescSlot(m_Device, s), DbgDescSlot(m_Device, u));
        }
    }
    if (set != static_cast<u64>(kInvalidSet))
        m_BoundSets[setIndex] = static_cast<DescriptorSetHandle>(set);
    D3D12_GPU_DESCRIPTOR_HANDLE s{}; s.ptr = srv;
    D3D12_GPU_DESCRIPTOR_HANDLE u{}; u.ptr = uav;
    D3D12_GPU_DESCRIPTOR_HANDLE m{}; m.ptr = sampler;
    SetRootTable(8  + setIndex, s);
    SetRootTable(16 + setIndex, u);
    SetRootTable(24 + setIndex, m);
}

void D3D12CommandList::ApplyResolvedRenderTargets(void* const* rtvs, u32 count, void* dsv) {
    if (!m_List) return;
    D3D12_CPU_DESCRIPTOR_HANDLE handles[8]{};
    const u32 n = (count < 8u) ? count : 8u;
    for (u32 i = 0; i < n; ++i) handles[i] = ToCpuHandle(rtvs ? rtvs[i] : nullptr);
    D3D12_CPU_DESCRIPTOR_HANDLE depth{};
    depth.ptr = reinterpret_cast<SIZE_T>(dsv);
    m_List->OMSetRenderTargets(n, (n && rtvs) ? handles : nullptr, FALSE, dsv ? &depth : nullptr);
    m_HasRtv = n > 0;
}

void D3D12CommandList::StreamLabel(RHICommandType type, const char* name) {
    RHICommandSetDebugLabel l{};
    if (name) std::strncpy(l.text, name, sizeof(l.text) - 1);
    Stream().Enqueue(type, l);
}

} // namespace he::rhi::d3d12

#endif // _WIN32
