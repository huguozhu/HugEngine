// ============================================================================
//  D3D11Device.cpp —— D3D11 设备
// ============================================================================
#include "D3D11Device.h"
#include "D3D11CommandList.h"
#include "D3D11Resources.h"
#include "D3D11SwapChain.h"

#include <cstring>
#include <cwchar>
#include <unordered_map>

namespace he::rhi::d3d11 {

D3D11Device::~D3D11Device() {
    if (m_Context) {
        m_Context->ClearState();
        m_Context->Flush();
    }
    DrainDebugMessages();
}

// ---------------------------------------------------------------------------
//  设备创建
// ---------------------------------------------------------------------------

/// 是否打开 D3D11 调试层：HE_D3D11_DEBUG_LAYER=0 / 1 显式指定；未指定时 Debug 构建默认打开
static bool WantDebugLayer() {
    if (const char* env = std::getenv("HE_D3D11_DEBUG_LAYER")) return env[0] != '0';
#if defined(_DEBUG)
    return true;
#else
    return false;
#endif
}

static std::string AdapterName(const DXGI_ADAPTER_DESC1& desc) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, s.data(), n, nullptr, nullptr);
    return s;
}

bool D3D11Device::Initialize() {
    if (!CreateDeviceAndContext()) return false;
    QueryCaps();
    return true;
}

bool D3D11Device::CreateDeviceAndContext() {
    if (!HE_D3D11_CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&m_Factory)), "CreateDXGIFactory2")) return false;

    // 选适配器：跳过 WARP（软件渲染），在支持特性级别 11_1 的适配器里按显存选最大的（与 D3D12 后端一致）
    const D3D_FEATURE_LEVEL kLevel = D3D_FEATURE_LEVEL_11_1;
    ComPtr<IDXGIAdapter1> adapter, best;
    SIZE_T bestVram = 0;
    DXGI_ADAPTER_DESC1 bestDesc{};
    for (UINT i = 0; m_Factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        // 设备与上下文都传空：只探测该适配器能否以 11_1 创建设备（成功返回 S_FALSE）
        if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                     &kLevel, 1, D3D11_SDK_VERSION, nullptr, nullptr, nullptr))) {
            continue;
        }
        if (!best || desc.DedicatedVideoMemory > bestVram) {
            bestVram = desc.DedicatedVideoMemory;
            best     = adapter;
            bestDesc = desc;
        }
    }
    if (!best) {
        HE_CORE_ERROR("[D3D11] 找不到支持特性级别 11_1 的硬件适配器（D3D11 后端要求 11_1：64 个 UAV 槽、带偏移的常量缓冲绑定）");
        return false;
    }
    HE_CORE_INFO("[D3D11] 使用适配器: {} (显存 {} MB)", AdapterName(bestDesc),
                 static_cast<u32>(bestDesc.DedicatedVideoMemory / (1024 * 1024)));

    // BGRA_SUPPORT：交换链与 ImGui 可能用 BGRA8；调试层按需打开，没装「图形工具」时退回无调试层
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const bool wantDebug = WantDebugLayer();
    ComPtr<ID3D11Device>        device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(best.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   flags | (wantDebug ? D3D11_CREATE_DEVICE_DEBUG : 0u),
                                   &kLevel, 1, D3D11_SDK_VERSION, &device, &got, &context);
    if (FAILED(hr) && wantDebug) {
        HE_CORE_WARN("[D3D11] 调试层不可用（需要 Windows 可选功能「图形工具」），改为不带调试层创建");
        hr = D3D11CreateDevice(best.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                               &kLevel, 1, D3D11_SDK_VERSION, &device, &got, &context);
    }
    if (!HE_D3D11_CHECK(hr, "D3D11CreateDevice")) return false;

    // D3D11.4 接口：ID3D11Fence（时间线围栏）需要 ID3D11Device5 / ID3D11DeviceContext4（Windows 10 1703+）
    if (FAILED(device.As(&m_Device)) || FAILED(context.As(&m_Context))) {
        HE_CORE_ERROR("[D3D11] 运行时缺少 D3D11.4 接口（ID3D11Device5 / ID3D11DeviceContext4），需要 Windows 10 1703 或更新版本");
        return false;
    }

    // 最低运行环境（规划 1.3 节）：缺任何一项都不启动，并列出全部缺项
    D3D11_FEATURE_DATA_D3D11_OPTIONS  opt{};
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 opt2{};
    m_Device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opt, sizeof(opt));
    m_Device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &opt2, sizeof(opt2));
    std::string missing;
    if (!opt.ConstantBufferOffsetting)              missing += " ConstantBufferOffsetting（常量环带偏移绑定）";
    if (!opt.MapNoOverwriteOnDynamicConstantBuffer) missing += " MapNoOverwriteOnDynamicConstantBuffer（常量环 NO_OVERWRITE 写入）";
    if (!opt2.TypedUAVLoadAdditionalFormats)        missing += " TypedUAVLoadAdditionalFormats（RGBA16F 等 UAV 读取）";
    if (!missing.empty()) {
        HE_CORE_ERROR("[D3D11] 适配器不满足 D3D11 后端的最低要求，缺少:{}", missing);
        return false;
    }

    m_Context.As(&m_Annotation);   // 调试标注（失败不影响运行）

    // 调试层消息：只记录、不中断（SetBreakOnSeverity 在没有调试器时会让进程永久等待，
    // D3D12 后端“静默挂起”教训 #1）。消息在每次提交后由 DrainDebugMessages 写进日志
    if (wantDebug && SUCCEEDED(m_Device.As(&m_InfoQueue))) {
        m_InfoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, FALSE);
        m_InfoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, FALSE);
        m_InfoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING, FALSE);
        HE_CORE_INFO("[D3D11] 调试层已启用，消息写入日志（HE_D3D11_DEBUG_LAYER=0 可关闭）");
    }
    return true;
}

void D3D11Device::QueryCaps() {
    m_Caps = DeviceCaps{};
    m_Caps.maxBindlessResources = 0;      // 没有 bindless：渲染层据此走逐 draw 绑定（规划 3.2）
    m_Caps.maxPushConstantsSize = 256;    // 推送常量由常量环提供，每槽 256 字节（规划 3.5）
    m_Caps.maxSamplerAnisotropy = D3D11_REQ_MAXANISOTROPY;
    // 以下能力 D3D11 / SM 5.0 都不具备，保持默认的 false：
    // 光追、mesh shader、DGC、异步计算、独立拷贝队列、VRS、Work Graphs 等

    // 多线程录制（M7）：驱动不原生支持命令列表时，deferred context 只是运行时模拟，没有收益
    D3D11_FEATURE_DATA_THREADING th{};
    m_Device->CheckFeatureSupport(D3D11_FEATURE_THREADING, &th, sizeof(th));
    HE_CORE_INFO("[D3D11] 设备就绪：特性级别 11_1，驱动原生命令列表={}，bindless=不支持，mesh shader=不支持",
                 th.DriverCommandLists ? "是" : "否");
}

void D3D11Device::OnGpuTimeout() {
    if (m_TimeoutReported) return;
    m_TimeoutReported = true;
    std::string list;
    for (u32 i = 0; i < kRecentPsoCount; ++i) {
        const char* n = m_RecentPsos[(m_RecentCursor + i) % kRecentPsoCount];
        if (n) { list += "\n    "; list += n; }
    }
    HE_CORE_ERROR("[D3D11] GPU 等待超时前最近执行的绘制 / 派发（旧 → 新）：{}", list);
    DrainDebugMessages();
}

// ---------------------------------------------------------------------------
//  调试层消息
// ---------------------------------------------------------------------------
void D3D11Device::DrainDebugMessages() {
    if (!m_InfoQueue) return;
    // 同一消息 ID 只打前 kMaxPerId 次（很多问题每帧都会重复报告）
    static constexpr u32 kMaxPerId = 5;
    static std::unordered_map<u32, u32> s_Counts;
    const UINT64 n = m_InfoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        if (FAILED(m_InfoQueue->GetMessage(i, nullptr, &len)) || len == 0) continue;
        std::vector<char> buf(len);
        auto* msg = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
        if (FAILED(m_InfoQueue->GetMessage(i, msg, &len))) continue;
        if (msg->Severity > D3D11_MESSAGE_SEVERITY_WARNING) continue;   // INFO / MESSAGE 不打
        u32& count = s_Counts[static_cast<u32>(msg->ID)];
        if (++count > kMaxPerId) continue;
        const char* suffix = (count == kMaxPerId) ? "（此后同一消息不再打印）" : "";
        if (msg->Severity == D3D11_MESSAGE_SEVERITY_WARNING) {
            HE_CORE_WARN("[D3D11 WARNING] id={} {}{}", static_cast<u32>(msg->ID), msg->pDescription, suffix);
        } else {
            HE_CORE_ERROR("[D3D11 ERROR] id={} {}{}", static_cast<u32>(msg->ID), msg->pDescription, suffix);
        }
    }
    m_InfoQueue->ClearStoredMessages();
}

// ---------------------------------------------------------------------------
//  资源创建（M2 骨架：返回占位对象，后续子任务替换为真正的 D3D11 资源）
// ---------------------------------------------------------------------------
std::unique_ptr<IRHISwapChain> D3D11Device::CreateSwapChain(const SwapChainDesc& desc) {
    auto sc = std::make_unique<D3D11SwapChain>();
    if (!sc->Initialize(this, desc)) return nullptr;
    return sc;
}

std::unique_ptr<IRHICommandList> D3D11Device::CreateCommandList(QueueType) {
    // 只有一个 immediate context：计算 / 拷贝队列的列表也录到同一个上下文里顺序执行
    return std::make_unique<D3D11CommandList>(this);
}

std::unique_ptr<IRHIBuffer> D3D11Device::CreateBuffer(const BufferDesc& desc) {
    auto buffer = std::make_unique<D3D11Buffer>();
    if (!buffer->Initialize(this, desc)) return nullptr;
    return buffer;
}

std::unique_ptr<IRHITexture> D3D11Device::CreateTexture(const TextureDesc& desc) {
    auto texture = std::make_unique<D3D11Texture>();
    if (!texture->Initialize(this, desc)) return nullptr;
    return texture;
}

std::unique_ptr<IRHISampler> D3D11Device::CreateSampler(const SamplerDesc& desc) {
    auto sampler = std::make_unique<D3D11Sampler>();
    if (!sampler->Initialize(this, desc)) return nullptr;
    return sampler;
}

// ---------------------------------------------------------------------------
//  视图登记表与逐 mip 视图
// ---------------------------------------------------------------------------
void D3D11Device::UnregisterTextureViews(D3D11Texture* texture) {
    for (auto it = m_Views.begin(); it != m_Views.end();) {
        it = (it->second.texture == texture) ? m_Views.erase(it) : std::next(it);
    }
}

void* D3D11Device::CreateTextureMipStorageView(IRHITexture* texture, u32 mipLevel, u32 arrayLayer) {
    auto* tex = static_cast<D3D11Texture*>(texture);
    if (!tex) return nullptr;
    if (HasAny(tex->GetUsage(), TextureUsage::RenderTarget)) return tex->GetRtv(mipLevel, arrayLayer);
    if (tex->IsDepth()) return tex->GetDsv(mipLevel, arrayLayer);
    return tex->GetUav(mipLevel, arrayLayer, true);
}

void* D3D11Device::CreateTextureMipSampledView(IRHITexture* texture, u32 mipLevel, u32 arrayLayer) {
    auto* tex = static_cast<D3D11Texture*>(texture);
    return tex ? tex->GetSrv(mipLevel, arrayLayer, true) : nullptr;
}

// ---------------------------------------------------------------------------
//  零缓冲与实例序号缓冲
// ---------------------------------------------------------------------------
ID3D11Buffer* D3D11Device::GetZeroVertexBuffer() {
    if (!m_ZeroVertexBuffer) {
        const u8 zeros[64] = {};   // 最大的顶点属性是 16 字节，64 字节足够
        D3D11_BUFFER_DESC bd{ sizeof(zeros), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
        D3D11_SUBRESOURCE_DATA init{ zeros, 0, 0 };
        HE_D3D11_CHECK(m_Device->CreateBuffer(&bd, &init, &m_ZeroVertexBuffer), "CreateBuffer(零缓冲)");
    }
    return m_ZeroVertexBuffer.Get();
}

ID3D11Buffer* D3D11Device::GetInstanceIndexBuffer() {
    if (!m_InstanceIndexBuffer) {
        std::vector<u32> indices(kInstanceIndexCount);
        for (u32 i = 0; i < kInstanceIndexCount; ++i) indices[i] = i;
        D3D11_BUFFER_DESC bd{ kInstanceIndexCount * 4u, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
        D3D11_SUBRESOURCE_DATA init{ indices.data(), 0, 0 };
        HE_D3D11_CHECK(m_Device->CreateBuffer(&bd, &init, &m_InstanceIndexBuffer), "CreateBuffer(实例序号缓冲)");
    }
    return m_InstanceIndexBuffer.Get();
}

// ---------------------------------------------------------------------------
//  持久映射缓冲
// ---------------------------------------------------------------------------
void D3D11Device::FlushOpenMaps() {
    for (D3D11Buffer* b : m_OpenMaps) b->FlushPersistentWrites();
}

void D3D11Device::RefreshOpenMaps() {
    for (D3D11Buffer* b : m_OpenMaps) b->RefreshPersistentReads();
}

std::unique_ptr<IRHIPipelineState> D3D11Device::CreatePipelineState(const PipelineStateDesc& desc) {
    // 着色器不可用（DXBC 编译失败）时也返回对象，只是标记为不可用（见 D3D11PipelineState 的说明）
    auto pso = std::make_unique<D3D11PipelineState>();
    pso->Initialize(this, desc);
    if (!pso->IsValid()) {
        HE_CORE_WARN("[D3D11] PSO {} 不可用：着色器没有可用的 DXBC（使用它的绘制 / 派发将被跳过）", pso->GetName());
    }
    return pso;
}

std::unique_ptr<IRHIQueryPool> D3D11Device::CreateQueryPool(u32 queryCount, QueryType type) {
    return std::make_unique<D3D11QueryPool>(queryCount, type);
}

// ---------------------------------------------------------------------------
//  句柄式资源
// ---------------------------------------------------------------------------
RHIBufferHandle D3D11Device::CreateBufferHandle(const BufferDesc& desc) { return m_Buffers.Add(CreateBuffer(desc)); }
void D3D11Device::DestroyBufferHandle(RHIBufferHandle handle) { m_Buffers.Remove(handle); }
IRHIBuffer* D3D11Device::ResolveBuffer(RHIBufferHandle handle) const { return m_Buffers.Get(handle); }

RHITextureHandle D3D11Device::CreateTextureHandle(const TextureDesc& desc) { return m_Textures.Add(CreateTexture(desc)); }
void D3D11Device::DestroyTextureHandle(RHITextureHandle handle) { m_Textures.Remove(handle); }
IRHITexture* D3D11Device::ResolveTexture(RHITextureHandle handle) const { return m_Textures.Get(handle); }

RHIPipelineStateHandle D3D11Device::CreatePipelineStateHandle(const PipelineStateDesc& desc) {
    return m_PipelineStates.Add(CreatePipelineState(desc));
}
void D3D11Device::DestroyPipelineStateHandle(RHIPipelineStateHandle handle) { m_PipelineStates.Remove(handle); }
IRHIPipelineState* D3D11Device::ResolvePipelineState(RHIPipelineStateHandle handle) const {
    return m_PipelineStates.Get(handle);
}

RHISamplerHandle D3D11Device::CreateSamplerHandle(const SamplerDesc& desc) { return m_Samplers.Add(CreateSampler(desc)); }
void D3D11Device::DestroySamplerHandle(RHISamplerHandle handle) { m_Samplers.Remove(handle); }
IRHISampler* D3D11Device::ResolveSampler(RHISamplerHandle handle) const { return m_Samplers.Get(handle); }

// ---------------------------------------------------------------------------
//  描述符集：只记录 binding → 资源（说明见头文件）
// ---------------------------------------------------------------------------
DescriptorSetLayoutHandle D3D11Device::CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) {
    m_SetLayouts.push_back(desc);
    return static_cast<DescriptorSetLayoutHandle>(m_SetLayouts.size());
}

DescriptorSetHandle D3D11Device::AllocateDescriptorSet(DescriptorSetLayoutHandle) {
    m_DescriptorSets.emplace_back();
    return static_cast<DescriptorSetHandle>(m_DescriptorSets.size());
}

D3D11Device::DescriptorEntry* D3D11Device::EntryFor(DescriptorSetHandle set, u32 binding) {
    if (set == 0 || set > m_DescriptorSets.size()) return nullptr;
    return &m_DescriptorSets[set - 1].entries[binding];
}

void D3D11Device::UpdateDescriptorSet(DescriptorSetHandle set, u32 binding, DescriptorType type, IRHIBuffer* buffer) {
    DescriptorEntry* e = EntryFor(set, binding);
    if (!e) return;
    e->type = type;
    e->buffers.assign(1, buffer);
}

void D3D11Device::UpdateDescriptorSet(DescriptorSetHandle set, u32 binding, DescriptorType type,
                                      IRHITexture* texture, IRHISampler* sampler) {
    DescriptorEntry* e = EntryFor(set, binding);
    if (!e) return;
    e->type = type;
    e->view = nullptr;
    // 独立采样器描述符只带采样器；其余带纹理（组合图像采样器同时带两者）
    if (type != DescriptorType::Sampler) e->textures.assign(1, texture);
    if (sampler || type == DescriptorType::Sampler) e->samplers.assign(1, sampler);
}

void D3D11Device::UpdateDescriptorSet(DescriptorSetHandle set, u32 binding, DescriptorType type,
                                      IRHITexture** textures, IRHISampler** samplers, u32 count) {
    DescriptorEntry* e = EntryFor(set, binding);
    if (!e) return;
    e->type = type;
    e->view = nullptr;
    if (textures) e->textures.assign(textures, textures + count);
    if (samplers) e->samplers.assign(samplers, samplers + count);
}

void D3D11Device::UpdateDescriptorSet(DescriptorSetHandle set, u32 binding, DescriptorType type,
                                      IRHIBuffer** buffers, u32 count) {
    DescriptorEntry* e = EntryFor(set, binding);
    if (!e || !buffers) return;
    e->type = type;
    e->buffers.assign(buffers, buffers + count);
}

void D3D11Device::UpdateDescriptorSetWithImageView(DescriptorSetHandle set, u32 binding, DescriptorType type,
                                                   void* imageView) {
    DescriptorEntry* e = EntryFor(set, binding);
    if (!e) return;
    e->type = type;
    e->view = imageView;   // 绑定时经视图登记表查回纹理与子资源
    e->textures.clear();
}

// ---------------------------------------------------------------------------
//  提交与同步
// ---------------------------------------------------------------------------
void D3D11Device::Submit(IRHICommandList* cmdList, RHICommandList*) {
    // immediate context 的命令在录制时就已发出；这里只下发本次提交的时间线信号（命令之后到达）
    if (cmdList) cmdList->Submit();
    // 持久映射缓冲：把本次提交里 GPU 写过的内容取回影子内存，CPU 下一帧读到的就是它
    RefreshOpenMaps();
    DrainDebugMessages();
}

void D3D11Device::SubmitAll(Span<IRHICommandList*> cmdLists) {
    for (auto* cl : cmdLists) Submit(cl);
}

/// 等围栏到达 value；timeoutMs 为 INFINITE 时一直等。成功返回 true
static bool WaitFenceValue(ID3D11Fence* fence, u64 value, DWORD timeoutMs) {
    if (fence->GetCompletedValue() >= value) return true;
    HANDLE ev = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!ev) return false;
    bool ok = false;
    if (SUCCEEDED(fence->SetEventOnCompletion(value, ev))) ok = (WaitForSingleObject(ev, timeoutMs) == WAIT_OBJECT_0);
    CloseHandle(ev);
    return ok;
}

void D3D11Device::WaitIdle() {
    if (!m_Device || !m_Context) return;
    if (!m_IdleFence && FAILED(m_Device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_IdleFence)))) return;
    m_Context->Signal(m_IdleFence.Get(), ++m_IdleValue);
    m_Context->Flush();
    // 限时 8 秒：GPU 侧出了非法操作时明确报错，而不是让进程永久挂起（D3D12 后端的教训）
    if (!WaitFenceValue(m_IdleFence.Get(), m_IdleValue, 8000)) {
        HE_CORE_ERROR("[D3D11] WaitIdle 等待 GPU 超时 8 秒，reason=0x{:08x}",
                      static_cast<u32>(m_Device->GetDeviceRemovedReason()));
    }
    DrainDebugMessages();
}

RHIFenceHandle D3D11Device::CreateFence() {
    ComPtr<ID3D11Fence> fence;
    if (!HE_D3D11_CHECK(m_Device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence")) {
        return kInvalidFence;
    }
    m_Fences.push_back(fence);
    return static_cast<RHIFenceHandle>(m_Fences.size());
}

void D3D11Device::DestroyFence(RHIFenceHandle fence) {
    if (fence > 0 && fence <= m_Fences.size()) m_Fences[fence - 1].Reset();
}

bool D3D11Device::WaitForFence(RHIFenceHandle fence, u64 value, u64 timeout) {
    ID3D11Fence* f = FindFence(fence);
    if (!f) return false;
    // timeout 以纳秒计（与 Vulkan 一致）；0 表示只查询不等待
    if (f->GetCompletedValue() >= value) return true;
    const DWORD ms = (timeout == UINT64_MAX) ? INFINITE : static_cast<DWORD>(timeout / 1000000ull);
    // 【必须先 Flush】immediate context 的 Signal 还在驱动的命令缓冲里、没交给 GPU 时，
    // 不 Flush 就去等它会永远等不到（实测 02.Cube 跑几帧后 CPU 0%、进程卡死）
    m_Context->Flush();
    return WaitFenceValue(f, value, ms);
}

u64 D3D11Device::GetFenceValue(RHIFenceHandle fence) const {
    ID3D11Fence* f = FindFence(fence);
    return f ? f->GetCompletedValue() : 0;
}

void D3D11Device::SignalFenceOnQueue(QueueType, RHIFenceHandle fence, u64 value) {
    if (ID3D11Fence* f = FindFence(fence)) m_Context->Signal(f, value);
}

void D3D11Device::WaitFenceOnQueue(QueueType, RHIFenceHandle, u64) {
    // 只有一个按序执行的 immediate context：早于本次调用发出的信号必然排在前面，GPU 侧等待
    // 不会带来任何额外顺序；而等的若是之后才发出的信号（异步计算的握手写法），GPU 会永久停住，
    // 下一次阻塞式回读随之死锁（实测 02.Cube 跑几帧后 CPU 0%）。因此不下发 GPU 侧等待
}

// ---------------------------------------------------------------------------
//  推送常量的常量环
// ---------------------------------------------------------------------------
bool D3D11Device::AllocatePushConstants(const void* data, u32& firstConstant, u64& generation) {
    if (!m_PushRing) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = kPushSlotSize * kPushSlotCount;
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (!HE_D3D11_CHECK(m_Device->CreateBuffer(&bd, nullptr, &m_PushRing), "CreateBuffer(推送常量环)")) return false;
        m_PushCursor = kPushSlotCount;   // 第一次分配走 DISCARD 分支
    }
    D3D11_MAP mapType = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (m_PushCursor >= kPushSlotCount) {
        // 用完一圈：DISCARD 换一块新内存（已发出的命令仍读旧内存），旧槽位从此作废
        mapType = D3D11_MAP_WRITE_DISCARD;
        m_PushCursor = 0;
        ++m_PushRingGeneration;
    }
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(m_Context->Map(m_PushRing.Get(), 0, mapType, 0, &ms))) return false;
    std::memcpy(static_cast<u8*>(ms.pData) + static_cast<size_t>(m_PushCursor) * kPushSlotSize, data, kPushSlotSize);
    m_Context->Unmap(m_PushRing.Get(), 0);
    firstConstant = m_PushCursor * (kPushSlotSize / 16u);
    generation    = m_PushRingGeneration;
    ++m_PushCursor;
    return true;
}

} // namespace he::rhi::d3d11
