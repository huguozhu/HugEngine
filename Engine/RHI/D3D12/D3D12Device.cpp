// ============================================================================
//  D3D12Device.cpp —— D3D12 设备的实现
// ============================================================================
#include "D3D12Device.h"
#include "D3D12SwapChain.h"
#include "D3D12CommandList.h"
#include "D3D12Resources.h"
#include "D3D12BindlessHeap.h"
#include <cstdlib>
#include <cstdio>
#include <algorithm>

#if defined(_WIN32)

namespace he::rhi::d3d12 {

D3D12Device::~D3D12Device() {
    // 退出前确保 GPU 空闲，否则销毁资源时驱动会报 use-after-free
    if (m_Queue) { m_Queue->Signal(nullptr, 0); }
    WaitIdle();
    // 调试层报告（退出时统一输出一次，避免刷屏）
    if (m_InfoQueue) {
        UINT64 n = m_InfoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < n; ++i) {
            SIZE_T len = 0;
            m_InfoQueue->GetMessage(i, nullptr, &len);
            if (!len) continue;
            std::vector<char> buf(len);
            auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            if (SUCCEEDED(m_InfoQueue->GetMessage(i, msg, &len)) &&
                msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR) {
                HE_CORE_ERROR("[D3D12] {}", msg->pDescription);
            }
        }
    }
}

bool D3D12Device::CreateDebugLayer() {
#if defined(HE_DEBUG)
    // 【可用性开关】调试层在部分驱动/环境下会让 Draw 调用变得极慢甚至卡住
    //（GPU-based validation 与 shader 补丁叠加时尤其明显）。
    // 设 HE_D3D12_NO_DEBUG_LAYER=1 可完全跳过调试层，用于确认"是调试层的问题
    // 还是我们自己的问题"。
    if (std::getenv("HE_D3D12_NO_DEBUG_LAYER")) {
        HE_CORE_INFO("[D3D12] 按 HE_D3D12_NO_DEBUG_LAYER 跳过调试层");
        return true;
    }
    // Debug 构建下打开调试层：能直接看到资源状态错误、描述符越界等
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&m_Debug)))) {
        m_Debug->EnableDebugLayer();
        // GPU 端验证（更严格，但会拖慢；阶段 0 先开，问题定位完再关）
        ComPtr<ID3D12Debug1> dbg1;
        if (SUCCEEDED(m_Debug.As(&dbg1))) {
            dbg1->SetEnableGPUBasedValidation(FALSE);
        }
        HE_CORE_INFO("[D3D12] 调试层已启用");
    } else {
        HE_CORE_WARN("[D3D12] 调试层不可用（未安装 Graphics Tools？）");
    }
#endif
    return true;
}

bool D3D12Device::CreateDeviceAndQueue() {
    UINT factoryFlags = 0;
#if defined(HE_DEBUG)
    factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_Factory)))) {
        // 调试工厂创建失败（未装调试层）时退回普通工厂
        HE_D3D12_CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&m_Factory)), "CreateDXGIFactory2");
        if (!m_Factory) return false;
    }

    // 选第一个支持 D3D12 的适配器（优先独显）
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIAdapter1> bestAdapter;
    SIZE_T bestVram = 0;
    for (UINT i = 0; m_Factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;   // 跳过 WARP
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))) {
            continue;   // 该适配器不支持 D3D12
        }
        if (desc.DedicatedVideoMemory > bestVram) {
            bestVram = desc.DedicatedVideoMemory;
            bestAdapter = adapter;
        }
    }
    if (!bestAdapter) {
        HE_CORE_ERROR("[D3D12] 找不到支持 D3D12 的适配器");
        return false;
    }
    {
        DXGI_ADAPTER_DESC1 desc{};
        bestAdapter->GetDesc1(&desc);
        HE_CORE_INFO("[D3D12] 使用适配器: {} (显存 {} MB)",
                     std::string(desc.Description, desc.Description + wcslen(desc.Description)).c_str(),
                     static_cast<u32>(desc.DedicatedVideoMemory / (1024 * 1024)));
    }

    if (!HE_D3D12_CHECK(D3D12CreateDevice(bestAdapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                          IID_PPV_ARGS(&m_Device)), "D3D12CreateDevice")) {
        return false;
    }

    // 调试消息队列（只记录 ERROR 级，避免刷屏）
    if (SUCCEEDED(m_Device->QueryInterface(IID_PPV_ARGS(&m_InfoQueue)))) {
        // 【注意 DenyList 的语义】早先把 ERROR 放进 DenyList，等于**屏蔽所有错误**，
        // 导致 PSO 创建失败时拿不到任何 D3D12 诊断信息（排查被拖了很久）。
        // 正确做法：DenyList 只放 INFO/MESSAGE 这类噪声级，ERROR/CORRUPTION 必须保留。
        D3D12_MESSAGE_SEVERITY denials[] = {
            D3D12_MESSAGE_SEVERITY_INFO,
            D3D12_MESSAGE_SEVERITY_MESSAGE,
        };
        D3D12_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumSeverities = 2;
        filter.DenyList.pSeverityList = denials;
        filter.DenyList.NumIDs = 0;
        filter.DenyList.pIDList = nullptr;
        m_InfoQueue->PushStorageFilter(&filter);
        // 【不要设断点】SetBreakOnSeverity(CORRUPTION, TRUE) 在没有调试器附加时
        // 会让进程**永久等待**（表现为 CPU 0%、窗口无响应、日志停在某个 API 调用上）。
        // 实测它让 01.Triangle 卡死在第一个 DrawInstanced 里。
        // 这里只记录消息、不中断执行。
        m_InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
        m_InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
    }

    // 描述符尺寸
    m_RtvDescSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_DsvDescSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    m_SrvDescSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // 图形命令队列
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (!HE_D3D12_CHECK(m_Device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_Queue)), "CreateCommandQueue")) {
        return false;
    }

    // 能力查询
    m_Caps.maxBindlessResources = 65536;
    m_Caps.maxPushConstantsSize = 256;      // D3D12 的 root constants 上限是 256 字节
    m_Caps.maxSamplerAnisotropy = 16;
    // 光追能力：硬件支持（RTX 4060 支持 DXR），但**本后端的 RTPipelineState /
    // BLAS / TLAS / SBT 属阶段 4，尚未实现**。此处必须上报 false ——
    // 否则样例会走光追路径并因 CreateRTPipelineState 返回空而中止，
    // 连带图形路径也跑不到（实测 01.Triangle 就卡在这里）。
    // 阶段 4 实现后把这一行改成真正的接口探测即可。
    m_Caps.supportsRayTracing = false;
    m_Caps.supportsMeshShaders = false;     // 阶段 5 用 D3D12_FEATURE_D3D12_OPTIONS7 查询
    m_Caps.supportsDGC = true;              // D3D12 原生支持 ExecuteIndirect
    HE_CORE_INFO("[D3D12] 设备就绪：bindless={} pushConst={} RT={}",
                 m_Caps.maxBindlessResources, m_Caps.maxPushConstantsSize,
                 m_Caps.supportsRayTracing ? "支持" : "不支持");
    return true;
}

bool D3D12Device::Initialize() {
    if (!CreateDebugLayer()) return false;
    if (!CreateDeviceAndQueue()) return false;
    return true;
}

void D3D12Device::ReportDeviceRemoved(const char* where) {
    if (!m_Device) return;
    const HRESULT reason = m_Device->GetDeviceRemovedReason();
    HE_CORE_ERROR("[D3D12] 设备丢失（{}）reason=0x{:08x} {}", where,
                  static_cast<u32>(reason), HrName(reason));
}

std::unique_ptr<IRHISwapChain> D3D12Device::CreateSwapChain(const SwapChainDesc& desc) {
    auto sc = std::make_unique<D3D12SwapChain>();
    if (!sc->Initialize(this, desc)) return nullptr;
    return sc;
}

std::unique_ptr<IRHICommandList> D3D12Device::CreateCommandList(QueueType queue) {
    auto cl = std::make_unique<D3D12CommandList>();
    if (!cl->Initialize(this, queue)) return nullptr;
    return cl;
}

std::unique_ptr<IRHICommandList> D3D12Device::CreateSecondaryCommandList() {
    return nullptr;   // 阶段 3（多线程录制）实现
}

std::unique_ptr<IRHIBuffer> D3D12Device::CreateBuffer(const BufferDesc& desc) {
    auto b = std::make_unique<D3D12Buffer>();
    if (!b->Initialize(this, desc)) return nullptr;
    return b;
}

std::unique_ptr<IRHITexture> D3D12Device::CreateTexture(const TextureDesc& desc) {
    auto t = std::make_unique<D3D12Texture>();
    if (!t->Initialize(this, desc)) return nullptr;
    return t;
}

std::unique_ptr<IRHISampler> D3D12Device::CreateSampler(const SamplerDesc& desc) {
    auto s = std::make_unique<D3D12Sampler>();
    if (!s->Initialize(this, desc)) return nullptr;
    return s;
}

std::unique_ptr<IRHIPipelineState> D3D12Device::CreatePipelineState(const PipelineStateDesc& desc) {
    auto p = std::make_unique<D3D12PipelineState>();
    if (!p->Initialize(this, desc)) return nullptr;
    return p;
}

void D3D12Device::WaitIdle() {
    if (!m_Queue || !m_Device) return;
    // 用一个临时围栏等待队列清空
    ComPtr<ID3D12Fence> fence;
    if (FAILED(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return;
    HANDLE ev = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!ev) return;
    m_Queue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, INFINITE);
    }
    CloseHandle(ev);
}

RHIFenceHandle D3D12Device::CreateFence() {
    FenceEntry e;
    if (FAILED(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&e.fence)))) {
        HE_CORE_ERROR("[D3D12] CreateFence 失败");
        return kInvalidFence;
    }
    u32 idx = 0;
    if (!m_FreeFences.empty()) {
        idx = m_FreeFences.back();
        m_FreeFences.pop_back();
        m_Fences[idx] = std::move(e);
    } else {
        m_Fences.push_back(std::move(e));
        idx = static_cast<u32>(m_Fences.size() - 1);
    }
    // 句柄 1-based：0 表示无效（kInvalidFence == 0）
    return static_cast<RHIFenceHandle>(idx + 1);
}

void D3D12Device::DestroyFence(RHIFenceHandle fence) {
    if (fence == kInvalidFence || fence > m_Fences.size()) return;
    const u32 idx = static_cast<u32>(fence - 1);
    m_Fences[idx].fence.Reset();
    m_Fences[idx].value = 0;
    m_FreeFences.push_back(idx);
}

bool D3D12Device::WaitForFence(RHIFenceHandle fence, u64 value, u64 timeout) {
    if (fence == kInvalidFence || fence > m_Fences.size()) return false;
    auto& e = m_Fences[static_cast<size_t>(fence - 1)];
    if (!e.fence) return false;
    if (e.fence->GetCompletedValue() >= value) return true;
    HANDLE ev = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!ev) return false;
    bool ok = false;
    if (SUCCEEDED(e.fence->SetEventOnCompletion(value, ev))) {
        ok = (WaitForSingleObject(ev, timeout == UINT64_MAX ? INFINITE : static_cast<DWORD>(timeout)) == WAIT_OBJECT_0);
    }
    CloseHandle(ev);
    return ok;
}

u64 D3D12Device::GetFenceValue(RHIFenceHandle fence) const {
    if (fence == kInvalidFence || fence > m_Fences.size()) return 0;
    const auto& e = m_Fences[static_cast<size_t>(fence - 1)];
    return e.fence ? e.fence->GetCompletedValue() : 0;
}

void D3D12Device::SignalFenceOnQueue(QueueType, RHIFenceHandle fence, u64 value) {
    if (fence == kInvalidFence || fence > m_Fences.size() || !m_Queue) return;
    auto& e = m_Fences[static_cast<size_t>(fence - 1)];
    if (e.fence) m_Queue->Signal(e.fence.Get(), value);
}

void D3D12Device::WaitFenceOnQueue(QueueType, RHIFenceHandle fence, u64 value) {
    if (fence == kInvalidFence || fence > m_Fences.size() || !m_Queue) return;
    auto& e = m_Fences[static_cast<size_t>(fence - 1)];
    if (e.fence) m_Queue->Wait(e.fence.Get(), value);
}

void D3D12Device::Submit(IRHICommandList* cmdList, RHICommandList* stream) {
    (void)stream;
    if (std::getenv("HE_D3D12_TRACE")) { std::printf("[TRACE] ...\n"); std::fflush(stdout); }
    if (!cmdList || !m_Queue) return;
    auto* cl = static_cast<D3D12CommandList*>(cmdList);
    ID3D12CommandList* lists[1] = { cl->GetNativeCommandList() };
    if (!lists[0]) return;
    m_Queue->ExecuteCommandLists(1, lists);
    // 每帧输出一次调试层的 ERROR（VS 的「输出」窗口内容进不了日志，这里补上）
    if (m_InfoQueue) {
        UINT64 n = m_InfoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < n; ++i) {
            SIZE_T len = 0;
            m_InfoQueue->GetMessage(i, nullptr, &len);
            if (!len) continue;
            std::vector<char> buf(len);
            auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            if (SUCCEEDED(m_InfoQueue->GetMessage(i, msg, &len)) &&
                msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR) {
                HE_CORE_ERROR("[D3D12 ERROR] id={} {}", static_cast<u32>(msg->ID), msg->pDescription);
            }
        }
        m_InfoQueue->ClearStoredMessages();
    }
}

void D3D12Device::SubmitAll(Span<IRHICommandList*> cmdLists) {
    for (auto* cl : cmdLists) Submit(cl);
}


// ===========================================================================
//  句柄式资源表
//
//  引擎的主力 API 是 `Create*Handle` + `Resolve*`（不是裸指针），
//  这些在 IRHIDevice 上都有**默认实现**（返回空句柄）—— 不覆盖就全都拿不到资源。
// ===========================================================================
RHIBufferHandle D3D12Device::CreateBufferHandle(const BufferDesc& desc) {
    auto obj = CreateBuffer(desc);
    if (!obj) return {};
    RHIBufferHandle h{};
    h.index = m_Buffers.Acquire(std::move(obj), h.generation);
    return h;
}

void D3D12Device::DestroyBufferHandle(RHIBufferHandle handle) {
    m_Buffers.Release(handle.index, handle.generation);
}

IRHIBuffer* D3D12Device::ResolveBuffer(RHIBufferHandle handle) const {
    return m_Buffers.Get(handle.index, handle.generation);
}

RHITextureHandle D3D12Device::CreateTextureHandle(const TextureDesc& desc) {
    auto obj = CreateTexture(desc);
    if (!obj) {
        HE_CORE_ERROR("D3D12Device::CreateTextureHandle: 纹理创建失败 {}x{} fmt={}",
                      desc.width, desc.height, static_cast<u32>(desc.format));
        return {};
    }
    RHITextureHandle h{};
    h.index = m_Textures.Acquire(std::move(obj), h.generation);
    return h;
}

void D3D12Device::DestroyTextureHandle(RHITextureHandle handle) {
    m_Textures.Release(handle.index, handle.generation);
}

IRHITexture* D3D12Device::ResolveTexture(RHITextureHandle handle) const {
    return m_Textures.Get(handle.index, handle.generation);
}

RHIPipelineStateHandle D3D12Device::CreatePipelineStateHandle(const PipelineStateDesc& desc) {
    auto obj = CreatePipelineState(desc);
    if (!obj) return {};
    RHIPipelineStateHandle h{};
    h.index = m_PipelineStates.Acquire(std::move(obj), h.generation);
    return h;
}

void D3D12Device::DestroyPipelineStateHandle(RHIPipelineStateHandle handle) {
    m_PipelineStates.Release(handle.index, handle.generation);
}

IRHIPipelineState* D3D12Device::ResolvePipelineState(RHIPipelineStateHandle handle) const {
    return m_PipelineStates.Get(handle.index, handle.generation);
}


// ===========================================================================
//  描述符集
//
//  D3D12 用「描述符堆 + 根签名参数」表达资源绑定，没有 Vulkan 的描述符集。
//  这里用「每个 set 占一段连续堆槽位」来模拟：
//    set.baseSlot = 分配到的起始槽位
//    binding N    = baseSlot + N
//  BindDescriptorSet 时把 baseSlot 处的槽位作为根签名的描述符表绑上去。
// ===========================================================================
// GetCbvSrvUavHeap 是头文件里的内联定义，这里不再重复实现

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Device::GetSlotCpuHandle(u32 slot) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h{};
    if (!m_CbvSrvUavHeap) return h;
    h = m_CbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * m_CbvSrvUavSize;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Device::GetSetGpuHandle(u32 slot) const {
    D3D12_GPU_DESCRIPTOR_HANDLE h{};
    if (!m_CbvSrvUavHeap) return h;
    h = m_CbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(slot) * m_CbvSrvUavSize;
    return h;
}

u32 D3D12Device::GetSetBaseSlot(DescriptorSetHandle set) const {
    if (set == kInvalidSet || set > m_DescriptorSets.size()) return 0xFFFFFFFFu;
    return m_DescriptorSets[static_cast<size_t>(set - 1)].baseSlot;
}


// 懒创建两个 shader-visible 堆（SRV 4096 槽 + 采样器 16 槽）。
// 根签名同时声明了 SRV 表与 SAMPLER 表 ⇒ 绑定时两个堆都得设置。
bool D3D12Device::EnsureDescriptorHeaps() {
    if (!m_Device) return false;
    if (!m_CbvSrvUavHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kDescriptorHeapCapacity;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(m_Device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_CbvSrvUavHeap)))) {
            HE_CORE_ERROR("D3D12Device: 创建 CBV_SRV_UAV 堆失败");
            return false;
        }
        m_CbvSrvUavSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    if (!m_SamplerHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        hd.NumDescriptors = 128;   // 覆盖 s0..s127
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(m_Device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_SamplerHeap)))) {
            HE_CORE_ERROR("D3D12Device: 创建 SAMPLER 堆失败");
            return false;
        }
        // 【必须填满所有槽】着色器里可能有 `SamplerState u_Samplers[64] : register(s6)`，
        // 即需要 s6..s69 连续 64 个**有效**采样器描述符。空槽（未 CreateSampler 的）
        // 会让 CreateGraphicsPipelineState 返回 E_INVALIDARG
        //（实测所有带采样器数组的着色器都失败，而 01.Triangle 没有采样器所以正常）。
        D3D12_SAMPLER_DESC sd{};
        sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sd.MipLODBias = 0.0f;
        sd.MaxAnisotropy = 1;
        sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sd.MinLOD = 0.0f;
        sd.MaxLOD = D3D12_FLOAT32_MAX;

        const UINT samplerInc = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_SamplerHeap->GetCPUDescriptorHandleForHeapStart();
        for (u32 i = 0; i < 128; ++i) {
            m_Device->CreateSampler(&sd, h);
            h.ptr += samplerInc;
        }
        HE_CORE_INFO("D3D12Device: SAMPLER 堆就绪（128 槽全部填充为默认线性采样器）");
    }
    return true;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Device::GetDefaultSamplerGpu() const {
    D3D12_GPU_DESCRIPTOR_HANDLE h{};
    if (m_SamplerHeap) h = m_SamplerHeap->GetGPUDescriptorHandleForHeapStart();
    return h;
}


// 常量缓冲环：每次取一个 256 字节槽，写 CBV 到 CBV 堆，返回两者。
bool D3D12Device::AllocateConstants(D3D12_GPU_VIRTUAL_ADDRESS& outGpuAddr,
                                    D3D12_GPU_DESCRIPTOR_HANDLE& outCbvHandle) {
    if (!m_Device) return false;

    // 首次使用：建上传缓冲 + CBV 堆
    if (!m_ConstantRing) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = static_cast<UINT64>(kConstantSlotCount) * kConstantSlotSize;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc = { 1, 0 };
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(m_Device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&m_ConstantRing)))) {
            HE_CORE_ERROR("D3D12Device: 常量缓冲环创建失败");
            return false;
        }
        D3D12_RANGE range{ 0, 0 };
        m_ConstantRing->Map(0, &range, &m_ConstantRingMapped);

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kConstantSlotCount;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(m_Device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_CbvHeap)))) {
            HE_CORE_ERROR("D3D12Device: CBV 堆创建失败");
            return false;
        }
        m_CbvIncrement = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        HE_CORE_INFO("D3D12Device: 常量缓冲环就绪（{} 槽 × {} 字节）", kConstantSlotCount, kConstantSlotSize);
    }

    if (m_NextCbvSlot >= kConstantSlotCount) m_NextCbvSlot = 0;   // 简单环形回绕
    const u32 slot = m_NextCbvSlot++;

    outGpuAddr = m_ConstantRing->GetGPUVirtualAddress() + static_cast<UINT64>(slot) * kConstantSlotSize;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    cpu = m_CbvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(slot) * m_CbvIncrement;

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
    cbv.BufferLocation = outGpuAddr;
    cbv.SizeInBytes = kConstantSlotSize;
    m_Device->CreateConstantBufferView(&cbv, cpu);

    outCbvHandle = m_CbvHeap->GetGPUDescriptorHandleForHeapStart();
    outCbvHandle.ptr += static_cast<UINT64>(slot) * m_CbvIncrement;
    return true;
}

/// 取某块常量缓冲的 CPU 映射指针（供 CommandList 写入）
void* D3D12Device::GetConstantSlotPtr(D3D12_GPU_VIRTUAL_ADDRESS gpuAddr) {
    if (!m_ConstantRingMapped || !m_ConstantRing) return nullptr;
    const UINT64 base = m_ConstantRing->GetGPUVirtualAddress();
    return static_cast<void*>(static_cast<unsigned char*>(m_ConstantRingMapped) + (gpuAddr - base));
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Device::GetCbvHeapGpuStart() const {
    D3D12_GPU_DESCRIPTOR_HANDLE h{};
    if (m_CbvHeap) h = m_CbvHeap->GetGPUDescriptorHandleForHeapStart();
    return h;
}

ID3D12DescriptorHeap* D3D12Device::GetCbvHeap() const { return m_CbvHeap.Get(); }

DescriptorSetLayoutHandle D3D12Device::CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) {
    D3D12SetLayout layout;
    layout.bindings = desc.bindings;
    // 槽位数 = 最大 binding + 该 binding 的 count（保守做法：每个 binding 按 count 展开）
    u32 maxSlot = 0;
    for (const auto& b : desc.bindings) {
        maxSlot = std::max(maxSlot, b.binding + (b.count ? b.count : 1));
    }
    layout.slotCount = maxSlot;
    m_SetLayouts.push_back(layout);
    return static_cast<DescriptorSetLayoutHandle>(m_SetLayouts.size());   // 1-based
}

void D3D12Device::DestroyDescriptorSetLayout(DescriptorSetLayoutHandle layout) {
    (void)layout;   // 阶段 2 再做真正的回收
}

DescriptorSetHandle D3D12Device::AllocateDescriptorSet(DescriptorSetLayoutHandle layout) {
    if (layout == kInvalidLayout || layout > m_SetLayouts.size()) return {};
    u32 li = static_cast<u32>(layout - 1);
    const u32 need = m_SetLayouts[li].slotCount ? m_SetLayouts[li].slotCount : 8;

    // 两个 shader-visible 堆（SRV + SAMPLER）都必须在绑定前就绪
    if (!EnsureDescriptorHeaps()) return {};

    if (m_NextFreeSlot + need > kDescriptorHeapCapacity) {
        HE_CORE_ERROR("D3D12Device: 描述符堆耗尽（需要 {}，剩余 {}）", need,
                      kDescriptorHeapCapacity - m_NextFreeSlot);
        return {};
    }
    D3D12Set s;
    s.baseSlot = m_NextFreeSlot;
    s.layout   = li;
    m_NextFreeSlot += need;
    m_DescriptorSets.push_back(s);

    return static_cast<DescriptorSetHandle>(m_DescriptorSets.size());     // 1-based
}

// ---- UpdateDescriptorSet：把资源写进 set 对应的堆槽位 ----
static D3D12_SRV_DIMENSION SrvDimFor(DescriptorType) { return D3D12_SRV_DIMENSION_BUFFER; }

void D3D12Device::UpdateDescriptorSet(DescriptorSetHandle set, u32 binding, DescriptorType type,
                                      IRHIBuffer* buffer) {
    if (set == kInvalidSet || set > m_DescriptorSets.size() || !buffer) return;
    const auto& s = m_DescriptorSets[static_cast<size_t>(set - 1)];
    auto* b = static_cast<D3D12Buffer*>(buffer);
    if (!b->GetResource()) return;

    D3D12_CPU_DESCRIPTOR_HANDLE dst = GetSlotCpuHandle(s.baseSlot + binding);
    const u64 size = b->GetSize() ? b->GetSize() : 256;

    if (type == DescriptorType::UniformBuffer) {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
        cbv.BufferLocation = b->GetResource()->GetGPUVirtualAddress();
        cbv.SizeInBytes    = static_cast<UINT>((size + 255) & ~255ull);
        m_Device->CreateConstantBufferView(&cbv, dst);
    } else {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.FirstElement = 0;
        srv.Buffer.NumElements  = static_cast<UINT>(size / 4);
        srv.Buffer.StructureByteStride = static_cast<UINT>(b->GetStride() ? b->GetStride() : 4);
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        m_Device->CreateShaderResourceView(b->GetResource(), &srv, dst);
    }
}

void D3D12Device::UpdateDescriptorSet(DescriptorSetHandle set, u32 binding, DescriptorType type,
                                      IRHITexture* texture, IRHISampler*) {
    if (set == kInvalidSet || set > m_DescriptorSets.size() || !texture) return;
    const auto& s = m_DescriptorSets[static_cast<size_t>(set - 1)];
    auto* tex = static_cast<D3D12Texture*>(texture);
    D3D12_CPU_DESCRIPTOR_HANDLE dst = GetSlotCpuHandle(s.baseSlot + binding);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_UNKNOWN;   // 用资源自身格式
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = tex->GetMipLevels();
    m_Device->CreateShaderResourceView(tex->GetResource(), &srv, dst);
}

void D3D12Device::UpdateDescriptorSet(DescriptorSetHandle set, u32 binding, DescriptorType type,
                                      IRHITexture** textures, IRHISampler**, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        UpdateDescriptorSet(set, binding + i, type, textures ? textures[i] : nullptr, nullptr);
    }
}

void D3D12Device::UpdateDescriptorSet(DescriptorSetHandle set, u32 binding, DescriptorType type,
                                      IRHIBuffer** buffers, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        UpdateDescriptorSet(set, binding + i, type, buffers ? buffers[i] : nullptr);
    }
}

void D3D12Device::UpdateDescriptorSet(DescriptorSetHandle, u32, DescriptorType, IRHIAccelerationStructure*) {}
void D3D12Device::UpdateDescriptorSetWithImageView(DescriptorSetHandle, u32, DescriptorType, void*) {}

} // namespace he::rhi::d3d12

#endif // _WIN32
