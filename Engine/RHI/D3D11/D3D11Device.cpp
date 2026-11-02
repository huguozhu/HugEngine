// ============================================================================
//  D3D11Device.cpp —— D3D11 设备
// ============================================================================
#include "D3D11Device.h"
#include "D3D11CommandList.h"
#include "D3D11Resources.h"
#include "D3D11SwapChain.h"

#include <d3dcompiler.h>   // D3DCompile：内置的参数整形 CS
#include <cstring>
#include <cwchar>
#include <unordered_map>

namespace he::rhi::d3d11 {

D3D11Device::~D3D11Device() {
    // 先停下 PSO 预编译线程（它在用设备创建对象）
    m_PrecompileStop.store(true, std::memory_order_release);
    if (m_PrecompileThread.joinable()) m_PrecompileThread.join();
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
    m_BindlessHeap.Attach(this);   // 纹理数组桶要用设备与 immediate context（M6）
    // 时间戳频率：先同步测一次（之后每帧由 OnPresent 更新），再开始第一个 disjoint 查询
    {
        D3D11_QUERY_DESC qd{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        ComPtr<ID3D11Query> q;
        if (SUCCEEDED(m_Device->CreateQuery(&qd, &q))) {
            m_Context->Begin(q.Get());
            m_Context->End(q.Get());
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            const ULONGLONG start = GetTickCount64();
            while (m_Context->GetData(q.Get(), &dj, sizeof(dj), 0) == S_FALSE && GetTickCount64() - start < 1000) {
                SwitchToThread();
            }
            if (dj.Frequency > 0) m_TimestampPeriodNs = static_cast<float>(1e9 / static_cast<double>(dj.Frequency));
            HE_CORE_INFO("[D3D11] 时间戳频率 {} Hz（每刻度 {:.3f} ns）", dj.Frequency, m_TimestampPeriodNs);
        }
    }
    BeginDisjoint();
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

void* D3D11Device::GetImGuiCommandBuffer(IRHICommandList* cmd) const {
    return cmd ? static_cast<D3D11CommandList*>(cmd)->PrepareExternalRendering() : nullptr;
}

// ---------------------------------------------------------------------------
//  PSO 预编译（M4.4）
// ---------------------------------------------------------------------------
void D3D11Device::PrecompileQueuePSO(const PipelineStateDesc& desc) {
    auto item = std::make_unique<PrecompileItem>();
    item->desc = desc;
    // 着色器字节码深拷贝：调用方传的往往是栈上 / 临时对象，后台创建时早已失效
    ShaderBytecode** slots[] = { &item->desc.vertexShader, &item->desc.pixelShader, &item->desc.computeShader,
                                 &item->desc.meshShader, &item->desc.amplificationShader };
    for (ShaderBytecode** slot : slots) {
        if (!*slot) continue;
        item->shaders.push_back(std::make_unique<ShaderBytecode>(**slot));
        *slot = item->shaders.back().get();
    }
    std::lock_guard<std::mutex> lock(m_PrecompileMutex);
    m_PrecompileQueue.push_back(std::move(item));
    m_PrecompileTotal.store(static_cast<u32>(m_PrecompileQueue.size()), std::memory_order_release);
}

void D3D11Device::StartPSOPrecompile() {
    if (m_PrecompileThread.joinable()) return;   // 已在运行 / 已跑完
    {
        std::lock_guard<std::mutex> lock(m_PrecompileMutex);
        if (m_PrecompileQueue.empty()) return;
    }
    m_PrecompileThread = std::thread([this]() {
        size_t i = 0;
        while (!m_PrecompileStop.load(std::memory_order_acquire)) {
            PrecompileItem* item = nullptr;
            {
                std::lock_guard<std::mutex> lock(m_PrecompileMutex);
                if (i >= m_PrecompileQueue.size()) break;
                item = m_PrecompileQueue[i++].get();
            }
            // 只用 ID3D11Device 的创建接口（线程安全），不碰 immediate context；对象建好即丢弃
            D3D11PipelineState pso;
            pso.Initialize(this, item->desc);
            m_PrecompileDone.fetch_add(1, std::memory_order_acq_rel);
        }
        HE_CORE_INFO("[D3D11] PSO 预编译完成：{} / {}", m_PrecompileDone.load(), m_PrecompileTotal.load());
    });
}

float D3D11Device::GetPSOPrecompileProgress() const {
    const u32 total = m_PrecompileTotal.load(std::memory_order_acquire);
    if (total == 0) return 1.0f;
    return static_cast<float>(m_PrecompileDone.load(std::memory_order_acquire)) / static_cast<float>(total);
}

void D3D11Device::BeginDisjoint() {
    ComPtr<ID3D11Query>& q = m_Disjoint[m_DisjointCursor];
    if (!q) {
        D3D11_QUERY_DESC qd{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        if (!HE_D3D11_CHECK(m_Device->CreateQuery(&qd, &q), "CreateQuery(TIMESTAMP_DISJOINT)")) return;
    }
    m_DisjointPending[m_DisjointCursor] = false;   // 重新开始会丢掉上一轮未取的结果
    m_Context->Begin(q.Get());
    m_DisjointActive = true;
}

void D3D11Device::OnPresent() {
    if (m_DisjointActive) {
        m_Context->End(m_Disjoint[m_DisjointCursor].Get());
        m_DisjointPending[m_DisjointCursor] = true;
        m_DisjointActive = false;
    }
    // 已完成的结果更新频率（不刷新、不等待；频率会随功耗状态变化，disjoint 的那一段不采用）
    for (u32 i = 0; i < kDisjointCount; ++i) {
        if (!m_DisjointPending[i]) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (m_Context->GetData(m_Disjoint[i].Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        m_DisjointPending[i] = false;
        if (!dj.Disjoint && dj.Frequency > 0) {
            m_TimestampPeriodNs = static_cast<float>(1e9 / static_cast<double>(dj.Frequency));
        }
    }
    m_DisjointCursor = (m_DisjointCursor + 1) % kDisjointCount;
    BeginDisjoint();
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
//  资源冲突解绑（规划 3.7）
// ---------------------------------------------------------------------------
/// 视图所属的资源（只取指针作比较：GetResource 会加引用，取到后立即释放）
static ID3D11Resource* ResourceOf(ID3D11View* view) {
    if (!view) return nullptr;
    ID3D11Resource* r = nullptr;
    view->GetResource(&r);
    if (r) r->Release();
    return r;
}

// ---------------------------------------------------------------------------
//  间接参数整形（规划 3.3）
// ---------------------------------------------------------------------------
/// 序号 ≥ 计数的命令 instanceCount 置 0（DrawIndexed 命令的第 2 个 u32）
static const char kClampIndirectCountCS[] = R"(
cbuffer Params : register(b0) { uint argsOffset; uint stride; uint maxDraw; uint countOffset; };
ByteAddressBuffer   CountBuf : register(t0);
RWByteAddressBuffer Args     : register(u0);
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= maxDraw) return;
    if (id.x >= CountBuf.Load(countOffset)) Args.Store(argsOffset + id.x * stride + 4u, 0u);
}
)";

bool D3D11Device::ClampIndirectCount(D3D11Buffer* countBuffer, u64 countOffset, ID3D11UnorderedAccessView* argsUav,
                                     u64 argsOffset, u32 stride, u32 maxDraw) {
    if (!countBuffer || !argsUav || maxDraw == 0) return false;
    // 第一次用到时编译着色器、建常量缓冲
    if (!m_ClampCS) {
        if (m_ClampCSFailed) return false;
        ComPtr<ID3DBlob> code, errors;
        const HRESULT hr = D3DCompile(kClampIndirectCountCS, sizeof(kClampIndirectCountCS) - 1, "ClampIndirectCount",
                                      nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(hr) || !HE_D3D11_CHECK(m_Device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
                                                                         nullptr, &m_ClampCS), "CreateComputeShader(参数整形)")) {
            HE_CORE_ERROR("[D3D11] 参数整形 CS 编译失败：{}",
                          errors ? static_cast<const char*>(errors->GetBufferPointer()) : "（无错误信息）");
            m_ClampCSFailed = true;
            return false;
        }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = 16;
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (!HE_D3D11_CHECK(m_Device->CreateBuffer(&bd, nullptr, &m_ClampParams), "CreateBuffer(参数整形常量)")) {
            m_ClampCS.Reset();
            m_ClampCSFailed = true;
            return false;
        }
    }

    // 计数所在的一段拷进暂存缓冲（结构化缓冲的拷贝区间要按步长对齐，所以拷整个元素）
    const u32 align = countBuffer->GetStructStride() ? countBuffer->GetStructStride() : 4u;
    const u64 begin = (countOffset / align) * align;
    const u32 need  = (align + 15u) & ~15u;
    if (m_CountScratchSize < need) {
        m_CountScratch.Reset();
        m_CountScratchSrv.Reset();
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = need;
        bd.Usage     = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (!HE_D3D11_CHECK(m_Device->CreateBuffer(&bd, nullptr, &m_CountScratch), "CreateBuffer(计数暂存)")) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format                = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension         = D3D11_SRV_DIMENSION_BUFFEREX;
        sd.BufferEx.NumElements  = need / 4u;
        sd.BufferEx.Flags        = D3D11_BUFFEREX_SRV_FLAG_RAW;
        if (!HE_D3D11_CHECK(m_Device->CreateShaderResourceView(m_CountScratch.Get(), &sd, &m_CountScratchSrv),
                            "CreateShaderResourceView(计数暂存)")) {
            m_CountScratch.Reset();
            return false;
        }
        m_CountScratchSize = need;
    }
    const D3D11_BOX box{ static_cast<UINT>(begin), 0, 0, static_cast<UINT>(begin + align), 1, 1 };
    m_Context->CopySubresourceRegion(m_CountScratch.Get(), 0, 0, 0, 0, countBuffer->GetBuffer(), 0, &box);

    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(m_Context->Map(m_ClampParams.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return false;
    const u32 params[4] = { static_cast<u32>(argsOffset), stride, maxDraw, static_cast<u32>(countOffset - begin) };
    std::memcpy(ms.pData, params, sizeof(params));
    m_Context->Unmap(m_ClampParams.Get(), 0);

    // 派发：CS 阶段的状态被改动，结束后解绑并让命令列表重新应用自己的状态
    ID3D11ShaderResourceView* srv = m_CountScratchSrv.Get();
    ID3D11Buffer* cb = m_ClampParams.Get();
    m_Context->CSSetShader(m_ClampCS.Get(), nullptr, 0);
    m_Context->CSSetConstantBuffers(0, 1, &cb);
    m_Context->CSSetShaderResources(0, 1, &srv);
    m_Context->CSSetUnorderedAccessViews(0, 1, &argsUav, nullptr);
    m_Context->Dispatch((maxDraw + 63u) / 64u, 1, 1);
    ID3D11ShaderResourceView* noSrv = nullptr;
    ID3D11UnorderedAccessView* noUav = nullptr;
    ID3D11Buffer* noCb = nullptr;
    m_Context->CSSetShaderResources(0, 1, &noSrv);
    m_Context->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    m_Context->CSSetConstantBuffers(0, 1, &noCb);
    TrackSrvs(2, 0, 1, &noSrv);   // CS 的 t0 已置空
    m_ContextOwner = nullptr;
    return true;
}

u32 D3D11Device::EstimateIndirectCount(D3D11Buffer* countBuffer, u64 countOffset, u32 maxDraw) {
    if (!countBuffer || maxDraw == 0) return maxDraw;
    // 同一计数缓冲 + 偏移共用一组回读（键里放缓冲地址与偏移）
    const u64 key = (reinterpret_cast<u64>(countBuffer) << 8) ^ countOffset;
    CountReadback& rb = m_CountReadbacks[key];

    // 1) 收取已完成的回读（不刷新、不等待；没好就下次再看）
    for (u32 i = 0; i < CountReadback::kRing; ++i) {
        if (!rb.pending[i]) continue;
        D3D11_MAPPED_SUBRESOURCE ms{};
        if (m_Context->Map(rb.staging[i].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms) != S_OK) continue;
        std::memcpy(&rb.lastCount, static_cast<const u8*>(ms.pData) + rb.offset, sizeof(u32));
        m_Context->Unmap(rb.staging[i].Get(), 0);
        rb.pending[i] = false;
        rb.known = true;
    }

    // 2) 发起本次的回读：把计数所在的元素拷进环里的下一块暂存缓冲（结构化缓冲要按步长整元素拷）
    const u32 align = countBuffer->GetStructStride() ? countBuffer->GetStructStride() : 4u;
    const u64 begin = (countOffset / align) * align;
    const u32 need  = (align + 15u) & ~15u;
    if (rb.size < need) {
        for (auto& s : rb.staging) s.Reset();
        for (bool& p : rb.pending) p = false;
        rb.size = need;
    }
    const u32 slot = rb.cursor;
    if (!rb.pending[slot]) {   // 该块的上一次回读还没取走就跳过本次（不覆盖未读结果）
        if (!rb.staging[slot]) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth      = rb.size;
            bd.Usage          = D3D11_USAGE_STAGING;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            HE_D3D11_CHECK(m_Device->CreateBuffer(&bd, nullptr, &rb.staging[slot]), "CreateBuffer(计数回读)");
        }
        if (rb.staging[slot]) {
            const D3D11_BOX box{ static_cast<UINT>(begin), 0, 0, static_cast<UINT>(begin + align), 1, 1 };
            m_Context->CopySubresourceRegion(rb.staging[slot].Get(), 0, 0, 0, 0, countBuffer->GetBuffer(), 0, &box);
            rb.offset = static_cast<u32>(countOffset - begin);
            rb.pending[slot] = true;
            rb.cursor = (slot + 1) % CountReadback::kRing;
        }
    }

    // 3) 循环条数：留出增长余量；还没有结果时先用一个保守的上限
    if (!rb.known) return std::min<u32>(maxDraw, 65536u);
    const u64 bound = u64(rb.lastCount) * 2u + 64u;
    return static_cast<u32>(std::min<u64>(maxDraw, bound));
}

void D3D11Device::TrackSrvs(u32 stage, u32 slot, u32 count, ID3D11ShaderResourceView* const* views) {
    if (stage >= kSrvStageCount) return;
    for (u32 i = 0; i < count && slot + i < kSrvSlotCount; ++i) {
        ID3D11Resource* r = ResourceOf(views[i]);
        m_SrvResources[stage][slot + i] = r;
        if (r) m_SrvHigh[stage] = std::max(m_SrvHigh[stage], slot + i + 1);
    }
}

void D3D11Device::UnbindSrvsOf(ID3D11Resource* const* resources, u32 count) {
    ID3D11ShaderResourceView* none = nullptr;
    for (u32 stage = 0; stage < kSrvStageCount; ++stage) {
        for (u32 slot = 0; slot < m_SrvHigh[stage]; ++slot) {
            ID3D11Resource* bound = m_SrvResources[stage][slot];
            if (!bound) continue;
            bool hit = false;
            for (u32 i = 0; i < count && !hit; ++i) hit = (resources[i] == bound);
            if (!hit) continue;
            if (stage == 0)      m_Context->VSSetShaderResources(slot, 1, &none);
            else if (stage == 1) m_Context->PSSetShaderResources(slot, 1, &none);
            else                 m_Context->CSSetShaderResources(slot, 1, &none);
            m_SrvResources[stage][slot] = nullptr;
        }
    }
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
    return std::make_unique<D3D11QueryPool>(this, queryCount, type);
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

/// 纹理的整资源 SRV / UAV（UAV 只对可作 UAV 的纹理建）：写入描述符集时取出并持有
static void CaptureTextureViews(IRHITexture* texture, ComPtr<ID3D11ShaderResourceView>& srv,
                                ComPtr<ID3D11UnorderedAccessView>& uav) {
    auto* t = static_cast<D3D11Texture*>(texture);
    srv = t ? t->GetSrv() : nullptr;
    uav = (t && t->CanUav()) ? t->GetUav() : nullptr;
}

static ComPtr<ID3D11SamplerState> CaptureSampler(IRHISampler* sampler) {
    return sampler ? ComPtr<ID3D11SamplerState>(static_cast<D3D11Sampler*>(sampler)->GetState()) : nullptr;
}

void D3D11Device::UpdateDescriptorSet(DescriptorSetHandle set, u32 binding, DescriptorType type,
                                      IRHITexture* texture, IRHISampler* sampler) {
    DescriptorEntry* e = EntryFor(set, binding);
    if (!e) return;
    e->type = type;
    // 独立采样器描述符只带采样器；其余带纹理（组合图像采样器同时带两者）
    if (type != DescriptorType::Sampler) {
        e->srvs.resize(1);
        e->uavs.resize(1);
        CaptureTextureViews(texture, e->srvs[0], e->uavs[0]);
    }
    if (sampler || type == DescriptorType::Sampler) e->samplers.assign(1, CaptureSampler(sampler));
}

void D3D11Device::UpdateDescriptorSet(DescriptorSetHandle set, u32 binding, DescriptorType type,
                                      IRHITexture** textures, IRHISampler** samplers, u32 count) {
    DescriptorEntry* e = EntryFor(set, binding);
    if (!e) return;
    e->type = type;
    if (textures) {
        e->srvs.resize(count);
        e->uavs.resize(count);
        for (u32 i = 0; i < count; ++i) CaptureTextureViews(textures[i], e->srvs[i], e->uavs[i]);
    }
    if (samplers) {
        e->samplers.resize(count);
        for (u32 i = 0; i < count; ++i) e->samplers[i] = CaptureSampler(samplers[i]);
    }
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
    // 经视图登记表查回纹理与子资源，现在就建好该子资源的 SRV / UAV 并持有
    e->srvs.assign(1, nullptr);
    e->uavs.assign(1, nullptr);
    if (const D3D11ViewInfo* vi = FindView(imageView)) {
        e->srvs[0] = vi->texture->GetSrv(vi->mip, vi->layer, vi->single);
        if (vi->texture->CanUav()) e->uavs[0] = vi->texture->GetUav(vi->mip, vi->layer, vi->single);
    } else if (imageView) {
        HE_CORE_WARN("[D3D11] UpdateDescriptorSetWithImageView：未登记的视图句柄 {}（set {}, binding {}）",
                     imageView, set, binding);
    }
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
