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
#include <cstring>
#include <algorithm>

#if defined(_WIN32)

namespace he::rhi::d3d12 {

// 设备存活检查（与 D3D12Resources.cpp 里的同名辅助函数用途一致）
// 设备丢失时打印 DRED（需 HE_D3D12_DRED=1 在建设备前开启）：
// 自动面包屑给出每个命令列表最后完成的操作，页错误给出出错的 GPU 虚拟地址。只打印一次。
/// 按资源描述与子资源范围构造纹理的 SRV / UAV 描述。
/// · 3D 纹理用 TEXTURE3D；立方体图整体视图用 TEXTURECUBE；指定了数组层的子资源视图用 TEXTURE2DARRAY
/// · 子资源视图（single）只暴露 (mip, layer) 一级；整体视图从 mip 0 起覆盖全部级
/// · 深度 / TYPELESS 格式映射到可读写的具体格式（DSV 格式不是合法的 SRV 格式，直接用会移除设备）
void BuildTextureViewDescs(const D3D12_RESOURCE_DESC& rd, u32 mip, u32 layer, bool single,
                           D3D12_SHADER_RESOURCE_VIEW_DESC& srv, D3D12_UNORDERED_ACCESS_VIEW_DESC& uav) {
    DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;   // UNKNOWN：沿用资源格式
    switch (rd.Format) {
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: f = DXGI_FORMAT_R16G16B16A16_FLOAT;    break;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     f = DXGI_FORMAT_R8G8B8A8_UNORM;        break;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     f = DXGI_FORMAT_B8G8R8A8_UNORM;        break;
        case DXGI_FORMAT_R32_TYPELESS:          f = DXGI_FORMAT_R32_FLOAT;             break;
        case DXGI_FORMAT_D32_FLOAT:             f = DXGI_FORMAT_R32_FLOAT;             break;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:     f = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
        case DXGI_FORMAT_D16_UNORM:             f = DXGI_FORMAT_R16_UNORM;             break;
        default: break;
    }
    const UINT mips   = rd.MipLevels;
    const UINT layers = rd.DepthOrArraySize;
    srv = {};
    srv.Format = f;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    uav = {};
    uav.Format = f;
    if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srv.Texture3D.MostDetailedMip = single ? mip : 0;
        srv.Texture3D.MipLevels       = single ? 1 : mips;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        uav.Texture3D.MipSlice    = mip;
        uav.Texture3D.FirstWSlice = 0;
        uav.Texture3D.WSize       = static_cast<UINT>(-1);   // 该级的全部深度切片
        return;
    }
    if (single && layers > 1) {
        // 某一层某一级（立方体图逐面、数组逐层）
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Texture2DArray.MostDetailedMip = mip;
        srv.Texture2DArray.MipLevels       = 1;
        srv.Texture2DArray.FirstArraySlice = layer;
        srv.Texture2DArray.ArraySize       = 1;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        uav.Texture2DArray.MipSlice        = mip;
        uav.Texture2DArray.FirstArraySlice = layer;
        uav.Texture2DArray.ArraySize       = 1;
        return;
    }
    if (!single && layers == 6) {
        // 立方体贴图（天空盒/IBL）必须用 TEXTURECUBE 维度建 SRV，否则采样恒为 0
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv.TextureCube.MostDetailedMip = 0;
        srv.TextureCube.MipLevels       = mips;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        uav.Texture2DArray.MipSlice        = mip;
        uav.Texture2DArray.FirstArraySlice = 0;
        uav.Texture2DArray.ArraySize       = layers;
        return;
    }
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MostDetailedMip = single ? mip : 0;
    srv.Texture2D.MipLevels       = single ? 1 : mips;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Texture2D.MipSlice = mip;
}

// 已登记的命令列表（DumpDeviceRemovedInfo 是自由函数，拿不到设备对象，因此用文件级表）
static std::vector<D3D12CommandList*> g_CommandLists;

bool FindRecordedEventNames(const ID3D12CommandList* native, u32 beginCount, u32 markerCount,
                            std::vector<std::string>& begins, std::vector<std::string>& markers) {
    for (auto* cl : g_CommandLists)
        if (cl && cl->FindEventNames(native, beginCount, markerCount, begins, markers)) return true;
    return false;
}

void DumpDeviceRemovedInfo(ID3D12Device* dev) {
    static bool s_dumped = false;
    if (s_dumped || !dev) return;
    s_dumped = true;
    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dred)))) return;
    // DRED 1.2：带面包屑上下文（调试标签字符串），可直接给出卡住的命令所在的 pass 与最近的绘制标记
    ComPtr<ID3D12DeviceRemovedExtendedData1> dred1;
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 bc1{};
    if (SUCCEEDED(dred.As(&dred1)) && SUCCEEDED(dred1->GetAutoBreadcrumbsOutput1(&bc1))) {
        auto narrow = [](const wchar_t* w) -> std::string {
            if (!w) return {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            std::string s(n > 1 ? static_cast<size_t>(n - 1) : 0u, '\0');
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
            return s;
        };
        // 摘要总是输出：没有后续明细时，能区分「全部执行完」与「没拿到 DRED 数据」
        u32 nodeCount = 0, pending = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE1* n = bc1.pHeadAutoBreadcrumbNode; n; n = n->pNext) {
            ++nodeCount;
            if ((n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0) < n->BreadcrumbCount) ++pending;
        }
        HE_CORE_ERROR("[DRED] 面包屑：{} 个命令列表记录，其中 {} 个未执行完", nodeCount, pending);
        for (const D3D12_AUTO_BREADCRUMB_NODE1* n = bc1.pHeadAutoBreadcrumbNode; n; n = n->pNext) {
            const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            if (done >= n->BreadcrumbCount) continue;   // 已全部完成的列表不是元凶
            // 面包屑序号 → 名字：运行时给了上下文字符串就用它；否则按「第 k 个事件 / 标记命令 =
            // 该次录制记下的第 k 个名字」对应（需开 HE_D3D12_DRED 时的事件名记录）
            std::unordered_map<UINT, std::string> ctx;
            for (UINT c = 0; c < n->BreadcrumbContextsCount; ++c)
                ctx[n->pBreadcrumbContexts[c].BreadcrumbIndex] = narrow(n->pBreadcrumbContexts[c].pContextString);
            if (ctx.empty()) {
                u32 nb = 0, nm = 0;
                for (UINT i = 0; i < n->BreadcrumbCount; ++i) {
                    if (n->pCommandHistory[i] == D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT) ++nb;
                    else if (n->pCommandHistory[i] == D3D12_AUTO_BREADCRUMB_OP_SETMARKER) ++nm;
                }
                std::vector<std::string> begins, markers;
                if (FindRecordedEventNames(n->pCommandList, nb, nm, begins, markers)) {
                    u32 bi = 0, mi = 0;
                    for (UINT i = 0; i < n->BreadcrumbCount; ++i) {
                        if (n->pCommandHistory[i] == D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT && bi < begins.size()) ctx[i] = begins[bi++];
                        else if (n->pCommandHistory[i] == D3D12_AUTO_BREADCRUMB_OP_SETMARKER && mi < markers.size()) ctx[i] = markers[mi++];
                    }
                }
            }
            // 走到卡住的位置：事件栈 = 所在 pass 的嵌套路径，最近的标记 = 最近的绘制标签
            std::vector<std::string> stack;
            std::string lastMarker;
            for (UINT i = 0; i < done && i < n->BreadcrumbCount; ++i) {
                const auto op = n->pCommandHistory[i];
                const auto it = ctx.find(i);
                if (op == D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT) stack.push_back(it != ctx.end() ? it->second : "?");
                else if (op == D3D12_AUTO_BREADCRUMB_OP_ENDEVENT && !stack.empty()) stack.pop_back();
                else if (op == D3D12_AUTO_BREADCRUMB_OP_SETMARKER && it != ctx.end()) lastMarker = it->second;
            }
            std::string path;
            for (const auto& s : stack) { if (!path.empty()) path += " / "; path += s; }
            HE_CORE_ERROR("[DRED] 列表={}（{}）完成 {}/{} 个操作；所在 pass：{}；最近标记：{}",
                          static_cast<const void*>(n->pCommandList), narrow(n->pCommandListDebugNameW),
                          done, n->BreadcrumbCount, path.empty() ? "-" : path,
                          lastMarker.empty() ? "-" : lastMarker);
            const UINT from = done >= 4 ? done - 4 : 0;
            const UINT to   = (done + 4 < n->BreadcrumbCount) ? done + 4 : n->BreadcrumbCount;
            for (UINT i = from; i < to; ++i) {
                const auto it = ctx.find(i);
                HE_CORE_ERROR("[DRED]   op[{}]={}{}{}", i, static_cast<int>(n->pCommandHistory[i]),
                              it != ctx.end() ? "  " + it->second : std::string(),
                              i == done ? "  <== 未完成" : "");
            }
        }
    } else if (D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc{}; SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc))) {
        // 旧系统（无 DRED 1.2）：只有操作序列
        for (const D3D12_AUTO_BREADCRUMB_NODE* n = bc.pHeadAutoBreadcrumbNode; n; n = n->pNext) {
            const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            if (done >= n->BreadcrumbCount) continue;   // 已全部完成的列表不是元凶
            HE_CORE_ERROR("[DRED] 列表={} 完成 {}/{} 个操作", static_cast<const void*>(n->pCommandList),
                          done, n->BreadcrumbCount);
            // 打印卡住位置前后的操作（D3D12_AUTO_BREADCRUMB_OP 枚举值）
            const UINT from = done >= 4 ? done - 4 : 0;
            const UINT to   = (done + 4 < n->BreadcrumbCount) ? done + 4 : n->BreadcrumbCount;
            for (UINT i = from; i < to; ++i) {
                HE_CORE_ERROR("[DRED]   op[{}]={}{}", i, static_cast<int>(n->pCommandHistory[i]),
                              i == done ? "  <== 未完成" : "");
            }
        }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)) && pf.PageFaultVA) {
        HE_CORE_ERROR("[DRED] 页错误 VA=0x{:x}", static_cast<u64>(pf.PageFaultVA));
        // 该地址附近仍存活 / 最近释放的分配（出现在「已释放」里通常意味着 GPU 还在用已销毁的资源）
        for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadExistingAllocationNode; a; a = a->pNext)
            HE_CORE_ERROR("[DRED]   存活分配 type={} name={}", static_cast<int>(a->AllocationType),
                          a->ObjectNameA ? a->ObjectNameA : "-");
        for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadRecentFreedAllocationNode; a; a = a->pNext)
            HE_CORE_ERROR("[DRED]   已释放分配 type={} name={}", static_cast<int>(a->AllocationType),
                          a->ObjectNameA ? a->ObjectNameA : "-");
    }
}

static void CheckDeviceAlive(ID3D12Device* dev, const char* where) {
    if (!dev) return;
    const HRESULT r = dev->GetDeviceRemovedReason();
    if (FAILED(r)) {
        HE_CORE_ERROR("[设备丢失] 在 {} 之后, reason=0x{:08x}", where, static_cast<u32>(r));
        DumpDeviceRemovedInfo(dev);
    }
}

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
    // 【重要】本工程并没有定义 HE_DEBUG 宏（全局搜不到），所以此前这段代码
    // 永远编译不进去 ⇒ 调试层从未启用 ⇒ PSO 创建失败时拿不到任何诊断信息。
    // 改成**环境变量按需启用**：
    //   HE_D3D12_DEBUG_LAYER=1  → 打开调试层（并保留全部消息，不做过滤）
    //   HE_D3D12_GPU_VALIDATION=1 → 额外打开 GPU 端验证（很慢，只在需要时用）
    static const bool s_wantDebug = (std::getenv("HE_D3D12_DEBUG_LAYER") != nullptr);
    if (!s_wantDebug) return true;
    if (std::getenv("HE_D3D12_NO_DEBUG_LAYER")) {
        HE_CORE_INFO("[D3D12] 按 HE_D3D12_NO_DEBUG_LAYER 跳过调试层");
        return true;
    }

    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&m_Debug)))) {
        m_Debug->EnableDebugLayer();
        ComPtr<ID3D12Debug1> dbg1;
        if (SUCCEEDED(m_Debug.As(&dbg1))) {
            dbg1->SetEnableGPUBasedValidation(std::getenv("HE_D3D12_GPU_VALIDATION") ? TRUE : FALSE);
        }
        HE_CORE_INFO("[D3D12] 调试层已启用");
    } else {
        HE_CORE_WARN("[D3D12] 调试层不可用（需要 Graphics Tools 可选功能）");
    }
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

    // HE_D3D12_DRED=1：开启 DRED 自动面包屑与页错误记录，设备挂起时能定位到具体命令
    if (std::getenv("HE_D3D12_DRED")) {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings)))) {
            dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            // 面包屑上下文（DRED 1.2）：记下 BeginEvent / SetMarker 的字符串，报告里据此给出 pass 名
            ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dredSettings1;
            if (SUCCEEDED(dredSettings.As(&dredSettings1)))
                dredSettings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            HE_CORE_INFO("[D3D12] DRED 已启用");
        }
    }

    if (!HE_D3D12_CHECK(D3D12CreateDevice(bestAdapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                          IID_PPV_ARGS(&m_Device)), "D3D12CreateDevice")) {
        return false;
    }

    // 调试消息队列（只记录 ERROR 级，避免刷屏）
    if (SUCCEEDED(m_Device->QueryInterface(IID_PPV_ARGS(&m_InfoQueue)))) {
        // 不做 Severity 过滤：PSO 校验失败的原因常常落在 WARNING/INFO 里，
        // 过滤掉就查不到（此前把 ERROR 放进 DenyList，等于屏蔽了全部错误）。
        HE_CORE_INFO("[D3D12] InfoQueue 已接入（不过滤消息）");
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
    // 最高支持的着色器模型（从高往低探测：传入的 HighestShaderModel 不被支持时 CheckFeatureSupport 失败）
    {
        D3D12_FEATURE_DATA_SHADER_MODEL sm{};
        const D3D_SHADER_MODEL candidates[] = { D3D_SHADER_MODEL_6_8, D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6,
                                               D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_0 };
        for (D3D_SHADER_MODEL c : candidates) {
            sm.HighestShaderModel = c;
            if (SUCCEEDED(m_Device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) break;
            sm.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(0);
        }
        m_HighestShaderModel = static_cast<u32>(sm.HighestShaderModel);
    }
    HE_CORE_INFO("[D3D12] 设备就绪：bindless={} pushConst={} RT={} 最高着色器模型=0x{:x}",
                 m_Caps.maxBindlessResources, m_Caps.maxPushConstantsSize,
                 m_Caps.supportsRayTracing ? "支持" : "不支持", m_HighestShaderModel);
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
    HE_CORE_INFO("[WaitIdle 进入]");
    m_Queue->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, ev);
        // 【不能无限等待】若 GPU 侧有未完成的非法操作，INFINITE 会让进程永久挂起、
        // 且不产生任何日志（实测 02.Cube 初始化末尾就是这样停住的）。
        // 改成有上限的等待，超时就明确报出来。
        const DWORD r = WaitForSingleObject(ev, 8000);
        if (r == WAIT_TIMEOUT) {
            HE_CORE_ERROR("[WaitIdle] 等待 GPU 超时 8 秒，队列可能已挂起");
            const HRESULT rr = m_Device->GetDeviceRemovedReason();
            HE_CORE_ERROR("[WaitIdle] GetDeviceRemovedReason=0x{:08x}", static_cast<u32>(rr));
        }
    }
    CloseHandle(ev);
    HE_CORE_INFO("[WaitIdle 完成]");
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
    if (TraceEnabled()) { std::printf("[TRACE] Submit\n"); std::fflush(stdout); }
    if (!cmdList || !m_Queue) return;
    auto* cl = static_cast<D3D12CommandList*>(cmdList);
    ID3D12CommandList* lists[1] = { cl->GetNativeCommandList() };
    if (!lists[0]) return;
    static const bool s_LogSubmit = (std::getenv("HE_SUBSEQ") != nullptr);   // 每帧调用，只读一次环境变量
    if (s_LogSubmit) HE_CORE_ERROR("[SUB] list={} draws={}", (void*)cl, cl->GetTotalDraws());
    cl->ApplyTimelineWaits();         // 本次提交要等的时间线值：先于命令执行
    m_Queue->ExecuteCommandLists(1, lists);
    cl->OnSubmitted(m_Queue.Get());   // 记录该列表当前帧槽位的完成围栏
    cl->ApplyTimelineSignals();       // 本次提交的时间线信号：命令执行完之后才到达
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

RHISamplerHandle D3D12Device::CreateSamplerHandle(const SamplerDesc& desc) {
    auto obj = CreateSampler(desc);
    if (!obj) {
        HE_CORE_ERROR("D3D12Device::CreateSamplerHandle: 采样器创建失败");
        return {};
    }
    RHISamplerHandle h{};
    h.index = m_Samplers.Acquire(std::move(obj), h.generation);
    return h;
}

void D3D12Device::DestroySamplerHandle(RHISamplerHandle handle) {
    m_Samplers.Release(handle.index, handle.generation);
}

IRHISampler* D3D12Device::ResolveSampler(RHISamplerHandle handle) const {
    return m_Samplers.Get(handle.index, handle.generation);
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

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Device::SetWriteHandle(D3D12Set& s, u32 slot) {
    ++s.version;   // 内容即将改变：已有快照全部过期
    D3D12_CPU_DESCRIPTOR_HANDLE h{};
    if (!m_SetStagingHeap || slot >= kSetRegionCapacity) return h;
    h = m_SetStagingHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * m_CbvSrvUavSize;
    return h;
}

u64 D3D12Device::GetSetVersion(DescriptorSetHandle set) const {
    if (set == kInvalidSet || set > m_DescriptorSets.size()) return 0;
    return m_DescriptorSets[static_cast<size_t>(set - 1)].version;
}

bool D3D12Device::SnapshotDescriptorSet(DescriptorSetHandle set, D3D12_GPU_DESCRIPTOR_HANDLE& srvOut,
                                        D3D12_GPU_DESCRIPTOR_HANDLE& uavOut, u32& chunkOut) {
    if (set == kInvalidSet || set > m_DescriptorSets.size()) return false;
    if (!m_CbvSrvUavHeap || !m_SetStagingHeap) return false;
    const D3D12Set& s = m_DescriptorSets[static_cast<size_t>(set - 1)];
    // 集在暂存堆里是连续的两段：[baseSlot, +slotCount) 为 SRV/CBV，紧接着同样长度的 UAV
    const u32 count = s.slotCount * 2u;
    if (count == 0 || count > kSetRingChunkSize) return false;

    // 当前块放不下：换到下一块。该块上一轮的使用者必须已在 GPU 上执行完，否则等它
    if (m_SetRingOffset + count > kSetRingChunkSize) {
        m_SetRingChunk  = (m_SetRingChunk + 1) % kSetRingChunkCount;
        m_SetRingOffset = 0;
        auto& uses = m_SetRingChunks[m_SetRingChunk].uses;
        for (auto& [fence, value] : uses) {
            if (!fence || fence->GetCompletedValue() >= value) continue;
            if (HANDLE ev = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS)) {
                fence->SetEventOnCompletion(value, ev);
                WaitForSingleObject(ev, 5000);   // 有上限：设备挂起时不至于永久阻塞
                CloseHandle(ev);
            }
        }
        uses.clear();
    }
    const u32 dstSlot = kSetRingBase + m_SetRingChunk * kSetRingChunkSize + m_SetRingOffset;
    m_SetRingOffset += count;

    D3D12_CPU_DESCRIPTOR_HANDLE src = m_SetStagingHeap->GetCPUDescriptorHandleForHeapStart();
    src.ptr += static_cast<SIZE_T>(s.baseSlot) * m_CbvSrvUavSize;
    m_Device->CopyDescriptorsSimple(count, GetSlotCpuHandle(dstSlot), src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    srvOut   = GetSetGpuHandle(dstSlot);
    uavOut   = GetSetGpuHandle(dstSlot + (s.uavBase - s.baseSlot));
    chunkOut = m_SetRingChunk;
    return true;
}

void D3D12Device::MarkSetRingChunks(u32 chunkMask, ID3D12Fence* fence, u64 value) {
    if (!fence) return;
    for (u32 c = 0; c < kSetRingChunkCount && chunkMask; ++c) {
        if (!(chunkMask & (1u << c))) continue;
        chunkMask &= ~(1u << c);
        auto& uses = m_SetRingChunks[c].uses;
        bool found = false;
        for (auto& u : uses) {
            if (u.first.Get() == fence) { u.second = std::max(u.second, value); found = true; break; }
        }
        if (!found) uses.emplace_back(fence, value);
    }
}

ID3D12Resource* D3D12Device::GetSetResource(DescriptorSetHandle set, u32 binding, bool& isBuffer,
                                            UINT* subresource) const {
    isBuffer = false;
    if (subresource) *subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (set == kInvalidSet || set > m_DescriptorSets.size()) return nullptr;
    const auto& s = m_DescriptorSets[static_cast<size_t>(set - 1)];
    if (auto it = s.textures.find(binding); it != s.textures.end()) {
        if (subresource) {
            if (auto si = s.subresources.find(binding); si != s.subresources.end()) *subresource = si->second;
        }
        return it->second.Get();
    }
    if (auto it = s.buffers.find(binding); it != s.buffers.end() && it->second.defaultHeap) {
        isBuffer = true;
        return it->second.resource.Get();
    }
    return nullptr;
}

u32 D3D12Device::GetSetSamplerBase(DescriptorSetHandle set) const {
    if (set == kInvalidSet || set > m_DescriptorSets.size()) return 0;
    return m_DescriptorSets[static_cast<size_t>(set - 1)].samplerBase;
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
        // 描述符集暂存堆：不可被着色器访问（CopyDescriptors 的源必须是这种堆）
        D3D12_DESCRIPTOR_HEAP_DESC sd{};
        sd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        sd.NumDescriptors = kSetRegionCapacity;
        sd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(m_Device->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&m_SetStagingHeap)))) {
            HE_CORE_ERROR("D3D12Device: 创建描述符集暂存堆失败");
            m_CbvSrvUavHeap.Reset();
            return false;
        }
        // bindless 保留区：放在堆尾，避免与普通描述符集分配冲突（4096 个槽）
        // 实验：可通过环境变量把 bindless 基址设为 0（绑到堆起点），用于判别是基址越界还是绑定本身
        m_BindlessSrvBase = std::getenv("HE_BINDLESS_BASE0") ? 0u : (kDescriptorHeapCapacity - 4096u);
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

    // 描述符集用的采样器堆：与 CBV_SRV_UAV 堆同容量、同槽位编号。
    // 每个槽先填默认采样器 —— 未绑定采样器的槽也必须是**有效描述符**，
    // 否则着色器的采样器数组会让 PSO 校验失败。
    if (!m_SetSamplerHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        hd.NumDescriptors = kSetSamplerCapacity;   // 采样器堆上限 4080
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(m_Device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_SetSamplerHeap)))) {
            HE_CORE_ERROR("D3D12Device: 创建描述符集 SAMPLER 堆失败");
            return false;
        }
        m_SetSamplerSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

        D3D12_SAMPLER_DESC sd{};
        sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sd.MaxAnisotropy = 1;
        sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sd.MinLOD = 0.0f;
        sd.MaxLOD = D3D12_FLOAT32_MAX;

        D3D12_CPU_DESCRIPTOR_HANDLE h = m_SetSamplerHeap->GetCPUDescriptorHandleForHeapStart();
        for (u32 i = 0; i < kSetSamplerCapacity; ++i) {
            m_Device->CreateSampler(&sd, h);
            h.ptr += m_SetSamplerSize;
        }
        // bindless 采样器保留区：放在描述符集采样器堆的尾部 1024 槽（与着色器 u_Samplers[1024] 对应；
        // 描述符集实测最多用四百多个槽，前 1024 槽足够）。
        // 必须与描述符集共用同一张堆 —— 命令列表只 SetDescriptorHeaps 这一张采样器堆，
        // 若 bindless 表指向别的堆，每次绘制都会触发 D3D12 错误 708，采样结果未定义甚至导致设备挂起。
        m_BindlessSmpBase = kSetSamplerCapacity - 1024u;
        HE_CORE_INFO("D3D12Device: 描述符集 SAMPLER 堆就绪（{} 槽）", kSetSamplerCapacity);
    }
    return true;
}

void D3D12Device::RegisterViewResource(UINT64 handle, ID3D12Resource* res) {
    if (!handle || !res) return;
    m_ViewResources[handle] = ViewInfo{ res, 0, 0, false };
    if (m_ResStates.find(res) == m_ResStates.end())
        m_ResStates[res] = D3D12_RESOURCE_STATE_COMMON;
}

void D3D12Device::RegisterSubresourceView(UINT64 handle, ID3D12Resource* res, u32 mip, u32 layer) {
    if (!handle || !res) return;
    m_ViewResources[handle] = ViewInfo{ res, mip, layer, true };
    if (m_ResStates.find(res) == m_ResStates.end())
        m_ResStates[res] = D3D12_RESOURCE_STATE_COMMON;
}

void D3D12Device::UnregisterViewResource(UINT64 handle) {
    if (!handle) return;
    const auto it = m_ViewResources.find(handle);
    if (it == m_ViewResources.end()) return;
    // 只有「整资源」视图随纹理一起注销时才清掉状态记录；子资源视图（逐 mip / 逐层）可能在
    // 纹理存活期间被单独销毁重建（Hi-Z 随尺寸重建、IBL 缓存的逐 mip 视图），此时清掉会让
    // 下一次屏障的 StateBefore 退回 COMMON 而与实际不符
    if (!it->second.subresource) m_ResStates.erase(it->second.res);
    m_ViewResources.erase(it);
}

ID3D12Resource* D3D12Device::FindViewResource(UINT64 handle) const {
    const auto it = m_ViewResources.find(handle);
    return it == m_ViewResources.end() ? nullptr : it->second.res;
}

const D3D12Device::ViewInfo* D3D12Device::FindViewInfo(UINT64 handle) const {
    const auto it = m_ViewResources.find(handle);
    return it == m_ViewResources.end() ? nullptr : &it->second;
}

D3D12_RESOURCE_STATES D3D12Device::GetResourceState(ID3D12Resource* res) const {
    const auto it = m_ResStates.find(res);
    return it == m_ResStates.end() ? D3D12_RESOURCE_STATE_COMMON : it->second;
}

void D3D12Device::SetResourceState(ID3D12Resource* res, D3D12_RESOURCE_STATES st) {
    if (res) m_ResStates[res] = st;
}


bool D3D12Device::PassStatsBeginQuery(ID3D12GraphicsCommandList* l, const char* tag) {
    if (!l || !m_Device) return false;
    if (!m_StatsHeap) {
        D3D12_QUERY_HEAP_DESC qh{};
        qh.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
        qh.Count = 64;
        if (FAILED(m_Device->CreateQueryHeap(&qh, IID_PPV_ARGS(&m_StatsHeap)))) return false;
    }
    if (!m_StatsBuf) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS) * 64;
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc = {1,0};
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(m_Device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_StatsBuf)))) return false;
    }
    if (m_StatsCount >= 64) { m_StatsOpen = false; return false; }
    m_StatsSlot = m_StatsCount;
    m_StatsTags[m_StatsSlot] = tag;
    l->BeginQuery(m_StatsHeap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, static_cast<UINT>(m_StatsSlot));
    ++m_StatsCount;
    m_StatsOpen = true;
    return true;
}

void D3D12Device::PassStatsEndQuery(ID3D12GraphicsCommandList* l) {
    if (!l || !m_StatsHeap || !m_StatsOpen || m_StatsCount <= 0) return;
    m_StatsOpen = false;
    const int slot = m_StatsCount - 1;
    l->EndQuery(m_StatsHeap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, static_cast<UINT>(slot));
    l->ResolveQueryData(m_StatsHeap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
                        static_cast<UINT>(slot), 1, m_StatsBuf.Get(),
                        static_cast<UINT64>(slot) * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
    m_StatsPending = true;
}

void D3D12Device::PassStatsRead(const char* tag) {
    if (!m_StatsPending || !m_StatsBuf || m_StatsCount <= 0) return;
    m_StatsPending = false;
    WaitIdle();
    void* mp = nullptr; D3D12_RANGE rr{0, 0};
    if (FAILED(m_StatsBuf->Map(0, &rr, &mp)) || !mp) return;
    const auto* base = static_cast<const D3D12_QUERY_DATA_PIPELINE_STATISTICS*>(mp);
    HE_CORE_ERROR("[STATS] ===== {} 个 pass（{}）=====", m_StatsCount, tag);
    for (int k = 0; k < m_StatsCount; ++k) {
        const auto& s = base[k];
        HE_CORE_ERROR("[STATS] #{} {} IA={}/{} CInv={} CPrim={} PSInvoc={}",
            k, m_StatsTags[k] ? m_StatsTags[k] : "?", (u64)s.IAVertices, (u64)s.IAPrimitives,
            (u64)s.CInvocations, (u64)s.CPrimitives, (u64)s.PSInvocations);
    }
    D3D12_RANGE wr{0, 0};
    m_StatsBuf->Unmap(0, &wr);
    m_StatsCount = 0;
}



namespace {
// 诊断用：把一张 16F 渲染目标拷到回读缓冲并统计非零像素与最大值
void ScanOneRt(ID3D12Device* dev, ID3D12CommandQueue* queue, ID3D12Resource* res,
               D3D12_RESOURCE_STATES before, const char* tag) {
    if (!dev || !queue || !res) return;
    const D3D12_RESOURCE_DESC rd = res->GetDesc();
    const u32 rowPitch = static_cast<u32>((rd.Width * 8 + 255) & ~255ull);
    const u64 totalSize = static_cast<u64>(rowPitch) * rd.Height;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = totalSize; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc = {1,0};
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> rb;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) return;
    ComPtr<ID3D12CommandAllocator> a; ComPtr<ID3D12GraphicsCommandList> l;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&l)))) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (b.Transition.StateBefore != b.Transition.StateAfter) l->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = rb.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = rd.Format;
    dst.PlacedFootprint.Footprint.Width = static_cast<UINT>(rd.Width);
    dst.PlacedFootprint.Footprint.Height = rd.Height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = rowPitch;
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = res;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = before;
    if (b.Transition.StateBefore != b.Transition.StateAfter) l->ResourceBarrier(1, &b);
    l->Close();
    ID3D12CommandList* lists[1] = { l.Get() };
    queue->ExecuteCommandLists(1, lists);
    // 等空闲
    ComPtr<ID3D12Fence> f; if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)))) return;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(f.Get(), 1);
    if (ev) { f->SetEventOnCompletion(1, ev); WaitForSingleObject(ev, 5000); CloseHandle(ev); }
    void* mp = nullptr; D3D12_RANGE rr{0,0};
    if (FAILED(rb->Map(0, &rr, &mp)) || !mp) return;
    const unsigned char* raw = static_cast<const unsigned char*>(mp);
    u64 total = 0, nz = 0; float mx = 0.0f;
    for (u32 y = 0; y < static_cast<u32>(rd.Height); y += 8) {
        const float* row = reinterpret_cast<const float*>(raw + static_cast<size_t>(y) * rowPitch);
        for (u32 x = 0; x < static_cast<u32>(rd.Width); x += 8) {
            const float r0 = row[x*4+0], g0 = row[x*4+1], b0 = row[x*4+2];
            ++total;
            if (r0 != 0.0f || g0 != 0.0f || b0 != 0.0f) ++nz;
            if (r0 > mx) mx = r0; if (g0 > mx) mx = g0; if (b0 > mx) mx = b0;
        }
    }
    rb->Unmap(0, nullptr);
    HE_CORE_ERROR("[SCAN-ALL] {} {}x{} 采样={} 非零={} max={}", tag, (u64)rd.Width, (u32)rd.Height,
        (u64)total, (u64)nz, mx);
}
}

void D3D12Device::ScanAllHdrTargets() {
    int idx = 0;
    for (auto& kv : m_ViewResources) {
        ID3D12Resource* r = kv.second.res;
        if (!r) continue;
        const D3D12_RESOURCE_DESC d = r->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) continue;
        if (d.Width != 1920 || d.Height != 1080) continue;
        if (static_cast<u32>(d.Format) != 10) continue;   // R16G16B16A16_FLOAT
        char tag[64];
        std::snprintf(tag, sizeof(tag), "res#%d", idx++);
        ScanOneRt(m_Device.Get(), m_Queue.Get(), r, D3D12_RESOURCE_STATE_RENDER_TARGET, tag);
    }
    HE_CORE_ERROR("[SCAN-ALL] 共扫描 {} 张 1920x1080 RGBA16F", idx);
}




namespace {
// 计算 DXIL 反射里一个类型的字节大小（HLSL 打包规则，只处理标量/向量/矩阵/结构体）
u32 TypeSize(ID3D12ShaderReflectionType* ty) {
    if (!ty) return 4;
    D3D12_SHADER_TYPE_DESC td{};
    if (FAILED(ty->GetDesc(&td))) return 4;
    if (td.Class == D3D_SVC_STRUCT) {
        u32 end = 0;
        for (UINT m = 0; m < td.Members; ++m) {
            D3D12_SHADER_TYPE_DESC md{};
            auto* mt = ty->GetMemberTypeByIndex(m);
            if (!mt || FAILED(mt->GetDesc(&md))) continue;
            u32 sz;
            if (md.Class == D3D_SVC_STRUCT) sz = TypeSize(mt);
            else sz = 4u * (md.Rows ? md.Rows : 1u) * (md.Columns ? md.Columns : 1u);
            const u32 elem = md.Elements ? md.Elements : 1u;
            sz *= elem;
            end = std::max(end, md.Offset + sz);
        }
        return end ? ((end + 3u) & ~3u) : 4u;
    }
    const u32 sz = 4u * (td.Rows ? td.Rows : 1u) * (td.Columns ? td.Columns : 1u);
    return sz ? sz : 4u;
}
}

void D3D12Device::NoteSetLayoutStrides(ID3D12ShaderReflection* refl,
                                       const std::vector<DescriptorSetLayoutHandle>& layouts) {
    if (!refl) return;
    D3D12_SHADER_DESC sd{};
    if (FAILED(refl->GetDesc(&sd))) return;
    for (UINT i = 0; i < sd.BoundResources; ++i) {
        D3D12_SHADER_INPUT_BIND_DESC bd{};
        if (FAILED(refl->GetResourceBindingDesc(i, &bd))) continue;
        const bool structured = bd.Type == D3D_SIT_STRUCTURED || bd.Type == D3D_SIT_UAV_RWSTRUCTURED ||
                                bd.Type == D3D_SIT_UAV_APPEND_STRUCTURED || bd.Type == D3D_SIT_UAV_CONSUME_STRUCTURED ||
                                bd.Type == D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER;
        // 对结构化缓冲，NumSamples 字段存的就是结构体步长
        if (!structured || bd.NumSamples == 0 || bd.Space >= layouts.size()) continue;
        const DescriptorSetLayoutHandle lh = layouts[bd.Space];
        if (lh == kInvalidLayout || lh > m_SetLayouts.size()) continue;
        const u32 li = static_cast<u32>(lh - 1);
        u32& known = m_SetLayouts[li].strides[bd.BindPoint];
        if (known == bd.NumSamples) continue;
        if (known != 0) {
            HE_CORE_WARN("D3D12Device: 布局 {} 的 binding {} 在不同着色器里步长不一致（{} → {}）",
                         li + 1, bd.BindPoint, known, bd.NumSamples);
        }
        known = bd.NumSamples;
        // 先写描述符、后建 PSO 是常态：按新步长重写该布局下所有已写入此 binding 的 set
        for (auto& s : m_DescriptorSets) {
            if (s.layout != li) continue;
            auto it = s.buffers.find(bd.BindPoint);
            if (it != s.buffers.end()) WriteSetBufferDescriptor(s, bd.BindPoint, it->second);
        }
    }
}


void D3D12Device::FlushPendingBindlessBuffers() {
    if (m_PendingBindlessBufs.empty()) return;
    if (!m_CbvSrvUavHeap || m_CbvSrvUavSize == 0) return;
    auto pending = std::move(m_PendingBindlessBufs);
    m_PendingBindlessBufs.clear();
    for (auto& [slot, buf] : pending) {
        if (buf) WriteBindlessBufferSrv(slot, buf);
    }
}

u32 D3D12Device::LookupStructStride(u32 registerIndex) const {
    const auto it = m_StructStrides.find(registerIndex);
    if (it != m_StructStrides.end() && it->second > 4u) return it->second;
    // 【兜底表】引擎的 GPU 结构体尺寸有 static_assert 保证（见 Render/Pipeline/Material.h），
    // 运行期 DXIL 反射拿不到这些大小（GetType()->GetDesc 直接失败），因此这里按寄存器号给出。
    //   t1 = GPULight(64)  t2 = GPUObjectData(208)  t3 = GPUShadowData(256)
    //   t7 = LightGridCell(8)  t8 = uint(4)
    switch (registerIndex) {
        case 1u: return 64u;
        case 2u: return 208u;
        case 3u: return 256u;
        case 7u: return 8u;
        case 8u: return 4u;
        default: return it != m_StructStrides.end() ? it->second : 0u;
    }
}

D3D12_VERTEX_BUFFER_VIEW D3D12Device::GetZeroVertexBufferView() {
    D3D12_VERTEX_BUFFER_VIEW v{};
    if (!m_ZeroVertexBuffer && m_Device) {
        // UPLOAD 堆即可：只读、体积极小；创建后写一次全零
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        hp.CreationNodeMask = 1;
        hp.VisibleNodeMask  = 1;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 64;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc = { 1, 0 };
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(m_Device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_ZeroVertexBuffer)))) {
            void* p = nullptr;
            D3D12_RANGE noRead{ 0, 0 };
            if (SUCCEEDED(m_ZeroVertexBuffer->Map(0, &noRead, &p)) && p) {
                std::memset(p, 0, 64);
                m_ZeroVertexBuffer->Unmap(0, nullptr);
            }
        }
    }
    if (m_ZeroVertexBuffer) {
        v.BufferLocation = m_ZeroVertexBuffer->GetGPUVirtualAddress();
        v.SizeInBytes    = 64;
        v.StrideInBytes  = 0;   // 每个顶点都读同一份 0
    }
    return v;
}

void* D3D12Device::CreateTextureMipStorageView(IRHITexture* texture, u32 mipLevel, u32 arrayLayer) {
    auto* tex = static_cast<D3D12Texture*>(texture);
    if (!m_Device || !tex || !tex->GetResource()) return nullptr;
    ID3D12Resource* res = tex->GetResource();
    const D3D12_RESOURCE_DESC rd = res->GetDesc();
    // 非渲染目标：compute 逐 mip 写入（Hi-Z 金字塔等），返回该子资源的 UAV 视图键；
    // 渲染目标仍返回 RTV（IBL 预过滤等逐 mip 渲染直接拿它当附件）
    if (!(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
        return CreateMipViewKey(res, mipLevel, arrayLayer, true);
    if (mipLevel >= rd.MipLevels || arrayLayer >= rd.DepthOrArraySize) return nullptr;

    if (!m_MipRtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = kMipRtvCapacity;
        if (FAILED(m_Device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_MipRtvHeap)))) return nullptr;
        m_MipRtvInc = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    u32 idx = 0;
    if (!m_MipRtvFree.empty()) { idx = m_MipRtvFree.back(); m_MipRtvFree.pop_back(); }
    else if (m_MipRtvNext < kMipRtvCapacity) { idx = m_MipRtvNext++; }
    else { HE_CORE_ERROR("D3D12Device: 逐 mip RTV 池耗尽（{}）", kMipRtvCapacity); return nullptr; }

    D3D12_CPU_DESCRIPTOR_HANDLE h = m_MipRtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(idx) * m_MipRtvInc;
    D3D12_RENDER_TARGET_VIEW_DESC vd{};
    vd.Format = rd.Format;
    vd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    vd.Texture2DArray.MipSlice = mipLevel;
    vd.Texture2DArray.FirstArraySlice = arrayLayer;
    vd.Texture2DArray.ArraySize = 1;
    m_Device->CreateRenderTargetView(res, &vd, h);
    // 命令列表靠「视图句柄 → 资源」反查来做屏障；同时记下子资源，供按视图写描述符
    RegisterSubresourceView(static_cast<UINT64>(h.ptr), res, mipLevel, arrayLayer);
    return reinterpret_cast<void*>(h.ptr);
}

void* D3D12Device::CreateMipViewKey(ID3D12Resource* res, u32 mipLevel, u32 arrayLayer, bool uav) {
    if (!m_Device || !res) return nullptr;
    const D3D12_RESOURCE_DESC rd = res->GetDesc();
    const bool is3D = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    if (mipLevel >= rd.MipLevels || (!is3D && arrayLayer >= rd.DepthOrArraySize)) return nullptr;
    if (uav && !(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) return nullptr;
    if (!m_MipViewHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kMipRtvCapacity;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;   // CPU 侧：只作句柄键，不被着色器访问
        if (FAILED(m_Device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_MipViewHeap)))) return nullptr;
        m_MipViewInc = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    u32 idx = 0;
    if (!m_MipViewFree.empty()) { idx = m_MipViewFree.back(); m_MipViewFree.pop_back(); }
    else if (m_MipViewNext < kMipRtvCapacity) { idx = m_MipViewNext++; }
    else { HE_CORE_ERROR("D3D12Device: 逐 mip 视图池耗尽（{}）", kMipRtvCapacity); return nullptr; }

    D3D12_CPU_DESCRIPTOR_HANDLE h = m_MipViewHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(idx) * m_MipViewInc;
    // 句柄位置上放一个合法的视图（内容不被使用，只保证句柄指向有效描述符）
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    BuildTextureViewDescs(rd, mipLevel, arrayLayer, true, sd, ud);
    if (uav) m_Device->CreateUnorderedAccessView(res, nullptr, &ud, h);
    else     m_Device->CreateShaderResourceView(res, &sd, h);
    RegisterSubresourceView(static_cast<UINT64>(h.ptr), res, mipLevel, arrayLayer);
    return reinterpret_cast<void*>(h.ptr);
}

void* D3D12Device::CreateTextureMipSampledView(IRHITexture* texture, u32 mipLevel, u32 arrayLayer) {
    auto* tex = static_cast<D3D12Texture*>(texture);
    if (!tex || !tex->GetResource()) return nullptr;
    return CreateMipViewKey(tex->GetResource(), mipLevel, arrayLayer, false);
}

void D3D12Device::DestroyTextureMipView(void* view) {
    if (!view) return;
    const SIZE_T p = reinterpret_cast<SIZE_T>(view);
    // 逐 mip 视图池（非渲染目标的 SRV / UAV 视图键）
    if (m_MipViewHeap && m_MipViewInc) {
        const SIZE_T vb = m_MipViewHeap->GetCPUDescriptorHandleForHeapStart().ptr;
        if (p >= vb && p < vb + static_cast<SIZE_T>(kMipRtvCapacity) * m_MipViewInc) {
            UnregisterViewResource(static_cast<UINT64>(p));
            m_MipViewFree.push_back(static_cast<u32>((p - vb) / m_MipViewInc));
            return;
        }
    }
    if (!m_MipRtvHeap || m_MipRtvInc == 0) return;
    const SIZE_T base = m_MipRtvHeap->GetCPUDescriptorHandleForHeapStart().ptr;
    if (p < base || p >= base + static_cast<SIZE_T>(kMipRtvCapacity) * m_MipRtvInc) return;   // 不是本池的视图
    UnregisterViewResource(static_cast<UINT64>(p));
    // RTV 描述符在 OMSetRenderTargets 录制时就被读取，回收后复用不影响已录制的命令
    m_MipRtvFree.push_back(static_cast<u32>((p - base) / m_MipRtvInc));
}

void D3D12Device::RegisterCommandList(D3D12CommandList* cl) {
    if (cl) g_CommandLists.push_back(cl);
}

void D3D12Device::UnregisterCommandList(D3D12CommandList* cl) {
    g_CommandLists.erase(std::remove(g_CommandLists.begin(), g_CommandLists.end(), cl), g_CommandLists.end());
}

void D3D12Device::SetResourceDebugName(IRHIBuffer* resource, const char* name) {
    if (auto* b = static_cast<D3D12Buffer*>(resource); b && b->GetResource() && name)
        b->GetResource()->SetName(Utf8ToWide(name).c_str());
}

void D3D12Device::SetResourceDebugName(IRHITexture* resource, const char* name) {
    if (auto* t = static_cast<D3D12Texture*>(resource); t && t->GetResource() && name)
        t->GetResource()->SetName(Utf8ToWide(name).c_str());
}

void D3D12Device::SetResourceDebugName(IRHIPipelineState* resource, const char* name) {
    if (auto* p = static_cast<D3D12PipelineState*>(resource); p && p->GetPso() && name)
        p->GetPso()->SetName(Utf8ToWide(name).c_str());
}

float D3D12Device::GetTimestampPeriod() {
    UINT64 freq = 0;
    if (!m_Queue || FAILED(m_Queue->GetTimestampFrequency(&freq)) || freq == 0) return 1.0f;
    return static_cast<float>(1.0e9 / static_cast<double>(freq));
}

std::unique_ptr<IRHIQueryPool> D3D12Device::CreateQueryPool(u32 queryCount, QueryType type) {
    auto pool = std::make_unique<D3D12QueryPool>();
    if (!pool->Initialize(this, queryCount, type)) {
        HE_CORE_ERROR("D3D12Device: 查询池创建失败（{} 个，类型 {}）", queryCount, static_cast<int>(type));
        return nullptr;
    }
    return pool;
}

void* D3D12Device::CreateImGuiDescriptorPool() {
    return EnsureDescriptorHeaps() ? static_cast<void*>(m_CbvSrvUavHeap.Get()) : nullptr;
}

void* D3D12Device::GetImGuiCommandBuffer(IRHICommandList* cmd) const {
    auto* cl = static_cast<D3D12CommandList*>(cmd);
    if (!cl) return nullptr;
    cl->PrepareForExternalRendering();
    return cl->GetNativeCommandList();
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Device::GetSrvHeapGpuStart() const {
    D3D12_GPU_DESCRIPTOR_HANDLE h{};
    if (m_CbvSrvUavHeap) h = m_CbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
    return h;
}


D3D12_GPU_DESCRIPTOR_HANDLE D3D12Device::GetBindlessSmpGpuHandle() const {
    D3D12_GPU_DESCRIPTOR_HANDLE h{};
    if (!m_SetSamplerHeap) return h;   // 必须是 BindDescriptorSet 里实际绑定的采样器堆
    h = m_SetSamplerHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(m_BindlessSmpBase) * m_SetSamplerSize;
    return h;
}

bool D3D12Device::WriteBindlessSampler(u32 slot, IRHISampler* sampler) {
    if (!m_Device || !sampler) return false;
    if (!EnsureDescriptorHeaps()) return false;
    auto* smp = static_cast<D3D12Sampler*>(sampler);
    if (m_BindlessSmpBase + slot >= kSetSamplerCapacity) return false;   // 超出 bindless 采样器保留区
    // 写进描述符集采样器堆的 bindless 保留区（与 GetBindlessSmpGpuHandle 同一张堆）
    D3D12_CPU_DESCRIPTOR_HANDLE dst = m_SetSamplerHeap->GetCPUDescriptorHandleForHeapStart();
    dst.ptr += static_cast<SIZE_T>(m_BindlessSmpBase + slot) * m_SetSamplerSize;
    m_Device->CopyDescriptorsSimple(1, dst, smp->GetDescriptor(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    return true;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Device::GetBindlessSrvGpuHandle() const {
    D3D12_GPU_DESCRIPTOR_HANDLE h{};
    if (!m_CbvSrvUavHeap) return h;   // 必须是真正被 SetDescriptorHeaps 绑定的那张堆
    h = m_CbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(m_BindlessSrvBase) * m_CbvSrvUavSize;
    return h;
}


void D3D12Device::DeferRetire(ComPtr<ID3D12Resource> res, ComPtr<ID3D12DescriptorHeap> heap) {
    m_Retired.emplace_back(std::move(res), std::move(heap));
}

void D3D12Device::FlushRetired() {
    // 只有 GPU 已完成之前所有提交时才算安全
    if (m_Retired.empty()) return;
    WaitIdle();
    m_Retired.clear();
}

bool D3D12Device::WriteBindlessSrv(u32 slot, IRHITexture* texture) {
    if (!m_Device || !texture) return false;
    if (!m_CbvSrvUavHeap || m_CbvSrvUavSize == 0) return false;   // 绑定的堆尚未创建
    if (!m_BindlessSrvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kBindlessMirrorCapacity;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(m_Device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_BindlessSrvHeap)))) return false;
        m_BindlessDescSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    auto* tex = static_cast<D3D12Texture*>(texture);
    D3D12_CPU_DESCRIPTOR_HANDLE dst = m_CbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
    dst.ptr += static_cast<SIZE_T>(m_BindlessSrvBase + slot) * m_CbvSrvUavSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    // 资源可能是 TYPELESS 格式，SRV 需要具体类型
    DXGI_FORMAT srvFmt = tex->GetResource()->GetDesc().Format;
    switch (srvFmt) {
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: srvFmt = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     srvFmt = DXGI_FORMAT_R8G8B8A8_UNORM;    break;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     srvFmt = DXGI_FORMAT_B8G8R8A8_UNORM;    break;
        case DXGI_FORMAT_R32_TYPELESS:          srvFmt = DXGI_FORMAT_R32_FLOAT;         break;
        case DXGI_FORMAT_D32_FLOAT:             srvFmt = DXGI_FORMAT_R32_FLOAT;         break;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:     srvFmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
        case DXGI_FORMAT_D16_UNORM:             srvFmt = DXGI_FORMAT_R16_UNORM;         break;
        default: break;
    }
    sd.Format = srvFmt;
    sd.ViewDimension = (tex->GetArrayLayers() == 6) ? D3D12_SRV_DIMENSION_TEXTURECUBE
                                                    : D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (sd.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE) sd.TextureCube.MipLevels = tex->GetMipLevels();
    else sd.Texture2D.MipLevels = tex->GetMipLevels();
    m_Device->CreateShaderResourceView(tex->GetResource(), &sd, dst);
    return true;
}

bool D3D12Device::WriteBindlessBufferSrv(u32 slot, IRHIBuffer* buffer) {
    if (!m_Device || !buffer) return false;
    // 堆尚未创建（初始化早期）⇒ 排队，等 BindDescriptorSet 时补写
    if (!m_CbvSrvUavHeap || m_CbvSrvUavSize == 0) {
        m_PendingBindlessBufs.emplace_back(slot, buffer);
        return false;
    }
    if (!m_BindlessSrvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kBindlessMirrorCapacity;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(m_Device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_BindlessSrvHeap)))) return false;
        m_BindlessDescSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    auto* buf = static_cast<D3D12Buffer*>(buffer);
    D3D12_CPU_DESCRIPTOR_HANDLE dst = m_CbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
    dst.ptr += static_cast<SIZE_T>(m_BindlessSrvBase + slot) * m_CbvSrvUavSize;
    // 【步长按区间决定】同一个缓冲会被写进四段区间（见 D3D12BindlessHeap::RegisterBuffer），
    // 每段在着色器里的元素类型不同，StructureByteStride 必须与之一致，否则按句柄取到的
    // 骨骼矩阵 / 实例变换会整体错位（缓冲本身通常不带 stride，以前一律退回 4 字节）：
    //   t1024.. u_InstanceTransforms   float4x4  64 字节
    //   t1088.. u_VisibleInstanceIndices uint     4 字节
    //   t1152.. u_BoneMatrices         float4x4  64 字节
    //   t1216.. u_Materials            GPUMaterialData 112 字节
    u32 stride = buf->GetStride() ? static_cast<u32>(buf->GetStride()) : 4u;
    if      (slot >= 1024u && slot < 1088u) stride = 64u;
    else if (slot >= 1088u && slot < 1152u) stride = 4u;
    else if (slot >= 1152u && slot < 1216u) stride = 64u;
    else if (slot >= 1216u && slot < 1280u) stride = 112u;
    // 缓冲还不到一个元素（如 4 字节计数缓冲落在材质区间）：建出来的视图会越过缓冲末尾，
    // 属于非法视图，该区间这个句柄本就不会被这样读取，直接跳过
    if (buf->GetSize() < stride) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_UNKNOWN;
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Buffer.FirstElement = 0;
    sd.Buffer.NumElements = static_cast<UINT>(buf->GetSize() / stride);
    sd.Buffer.StructureByteStride = stride;
    m_Device->CreateShaderResourceView(buf->GetNativeHandle(), &sd, dst);
    return true;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Device::GetSetSamplerGpuHandle(u32 slot) const {
    D3D12_GPU_DESCRIPTOR_HANDLE h{};
    if (!m_SetSamplerHeap) return h;
    h = m_SetSamplerHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(slot) * m_SetSamplerSize;
    return h;
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
    if (m_CbvSrvUavHeap) h = m_CbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
    return h;
}

ID3D12DescriptorHeap* D3D12Device::GetCbvHeap() const { return m_CbvHeap.Get(); }

u32 D3D12Device::GetSetUavOffset(DescriptorSetHandle set) const {
    if (set == kInvalidSet || set > m_DescriptorSets.size()) return 0;
    return m_DescriptorSets[static_cast<size_t>(set - 1)].slotCount;
}

DescriptorSetLayoutHandle D3D12Device::CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) {
    D3D12SetLayout layout;
    layout.bindings = desc.bindings;
    // 槽位数 = 最大 binding + 该 binding 的 count（保守做法：每个 binding 按 count 展开）
    u32 maxSlot = 0;
    for (const auto& b : desc.bindings) {
        maxSlot = std::max(maxSlot, b.binding + (b.count ? b.count : 1));
    }
    layout.slotCount = maxSlot;
    // 采样器跨度：只统计采样器 / 组合图像采样器。bindless 采样器数组（count 很大）在 D3D12 下
    // 走 space2 的独立区间，不占描述符集的采样器范围，按 1 计即可。
    for (const auto& b : desc.bindings) {
        if (b.type != DescriptorType::Sampler && b.type != DescriptorType::CombinedImageSampler) continue;
        const u32 cnt = (b.count > 1 && b.count <= 64) ? b.count : 1u;
        layout.samplerSpan = std::max(layout.samplerSpan, b.binding + cnt);
    }
    m_SetLayouts.push_back(layout);
    return static_cast<DescriptorSetLayoutHandle>(m_SetLayouts.size());   // 1-based
}

void D3D12Device::DestroyDescriptorSetLayout(DescriptorSetLayoutHandle layout) {
    (void)layout;   // 阶段 2 再做真正的回收
}

DescriptorSetHandle D3D12Device::AllocateDescriptorSet(DescriptorSetLayoutHandle layout) {
    if (layout == kInvalidLayout || layout > m_SetLayouts.size()) return {};
    u32 li = static_cast<u32>(layout - 1);
    const u32 perSet = m_SetLayouts[li].slotCount ? m_SetLayouts[li].slotCount : 8;
    // 每个 set 要两块区域：前面放 SRV/CBV，后面放 UAV（同一张 CBV_SRV_UAV 堆）
    const u32 need = perSet * 2;

    // 两个 shader-visible 堆（SRV + SAMPLER）都必须在绑定前就绪
    if (!EnsureDescriptorHeaps()) return {};

    // 单个集的快照必须放得进环形区的一块
    if (need > kSetRingChunkSize) {
        HE_CORE_ERROR("D3D12Device: 描述符集过大（{} 槽，快照块只有 {} 槽）", need, kSetRingChunkSize);
        return {};
    }
    if (m_NextFreeSlot + need > kSetRegionCapacity) {
        HE_CORE_ERROR("D3D12Device: 描述符堆耗尽（需要 {}，剩余 {}）", need,
                      kSetRegionCapacity - m_NextFreeSlot);
        return {};
    }
    D3D12Set s;
    s.baseSlot  = m_NextFreeSlot;
    s.uavBase   = m_NextFreeSlot + perSet;
    s.slotCount = perSet;
    s.layout    = li;
    m_NextFreeSlot += need;
    // 【采样器按 set 独立分配】此前所有 set 的采样器都按 binding 号写在采样器堆的同一组槽位，
    // 不同 set 的同号 binding 互相覆盖（如天空盒的 binding 10 被 PBR 的阴影采样器覆盖，
    // 立方体贴图用错采样器 ⇒ 天空一片灰）。现在每个 set 在采样器堆里占自己的一段。
    const u32 span = m_SetLayouts[li].samplerSpan;
    if (span > 0) {
        if (m_NextSamplerSlot + span <= m_BindlessSmpBase) {
            s.samplerBase = m_NextSamplerSlot;
            m_NextSamplerSlot += span;
        } else {
            // 采样器堆不够时退回共享槽位（旧行为），并提示一次
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                HE_CORE_WARN("D3D12Device: 描述符集采样器槽位耗尽（已用 {} / {}），后续 set 退回共享槽位",
                             m_NextSamplerSlot, m_BindlessSmpBase);
            }
            s.samplerBase = 0;
        }
    }
    m_DescriptorSets.push_back(s);

    return static_cast<DescriptorSetHandle>(m_DescriptorSets.size());     // 1-based
}

// ---- UpdateDescriptorSet：把资源写进 set 对应的堆槽位 ----
static D3D12_SRV_DIMENSION SrvDimFor(DescriptorType) { return D3D12_SRV_DIMENSION_BUFFER; }

// ---------------------------------------------------------------------------
//  把资源写进描述符集对应的堆槽位
//
//  【关键】引擎用 `BindDescriptorSet(setIndex, set)` 的 setIndex 表示描述符集，
//  而 Slang/DXC 把它翻译成 HLSL 的 `register(tN, space<setIndex>)`。
//  因此每个 set 对应一个**寄存器空间**，根签名必须为每个 space 各声明一张表。
//  另外采样器必须写进独立的 SAMPLER 堆（CBV_SRV_UAV 堆放不下）。
// ---------------------------------------------------------------------------
void D3D12Device::UpdateDescriptorSet(DescriptorSetHandle set, u32 binding, DescriptorType type,
                                      IRHIBuffer* buffer) {
    if (set == kInvalidSet || set > m_DescriptorSets.size() || !buffer) return;
    if (!EnsureDescriptorHeaps()) return;

    auto* b = static_cast<D3D12Buffer*>(buffer);
    if (!b->GetResource()) return;
    auto& s = m_DescriptorSets[static_cast<size_t>(set - 1)];
    // 记下这个 binding 写的是哪个缓冲：布局之后从 PSO 反射学到（或修正）步长时据此重写
    s.textures.erase(binding);   // 同一 binding 改写为缓冲：清掉旧的纹理记录
    s.subresources.erase(binding);
    D3D12Set::BufferBinding& bb = s.buffers[binding];
    bb.resource     = b->GetResource();
    bb.size         = b->GetSize() ? b->GetSize() : 256;
    bb.bufferStride = static_cast<u32>(b->GetStride());
    bb.type         = type;
    D3D12_HEAP_PROPERTIES hp{};
    // DEFAULT 与 CUSTOM（CPU 可读写的 UAV 缓冲）堆上的缓冲都按普通资源状态规则转换；UPLOAD/READBACK 堆不行
    bb.defaultHeap  = SUCCEEDED(bb.resource->GetHeapProperties(&hp, nullptr)) &&
                      (hp.Type == D3D12_HEAP_TYPE_DEFAULT || hp.Type == D3D12_HEAP_TYPE_CUSTOM);
    WriteSetBufferDescriptor(s, binding, bb);
}

void D3D12Device::WriteSetBufferDescriptor(D3D12Set& s, u32 binding, const D3D12Set::BufferBinding& bb) {
    ID3D12Resource* res = bb.resource.Get();
    if (!res) return;
    D3D12_CPU_DESCRIPTOR_HANDLE dst = SetWriteHandle(s, s.baseSlot + binding);
    const u64 size = bb.size;

    if (bb.type == DescriptorType::UniformBuffer) {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
        cbv.BufferLocation = res->GetGPUVirtualAddress();
        cbv.SizeInBytes    = static_cast<UINT>((size + 255) & ~255ull);
        m_Device->CreateConstantBufferView(&cbv, dst);
        return;
    }

    // 【步长】StructureByteStride 必须等于着色器里的结构体大小，否则按下标取到的元素整体错位
    //（GPU 验证 id=1387）。引擎的 BufferDesc 不带步长，按优先级取：
    //   1) 该 set 所属布局从 PSO 反射记下的步长（NoteSetLayoutStrides，按「布局 + binding」）
    //   2) 缓冲自身的步长
    //   3) 旧的按寄存器号兜底表（布局尚未被任何 PSO 使用时的临时值，PSO 创建后会被重写）
    u32 stride = 0;
    const auto& layout = m_SetLayouts[s.layout];
    if (auto it = layout.strides.find(binding); it != layout.strides.end()) stride = it->second;
    if (!stride) stride = bb.bufferStride;
    if (!stride) stride = LookupStructStride(binding);
    if (!stride) stride = 4u;
    // 元素数必须 ≥1：缓冲比 stride 小时 size/stride=0，CreateShaderResourceView 判非法
    // 会直接把设备移除（实测 hr=0x887a0005）。
    const UINT nElems = std::max<UINT>(1u, static_cast<UINT>(size / stride));

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement = 0;
    srv.Buffer.NumElements  = nElems;
    srv.Buffer.StructureByteStride = stride;
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    m_Device->CreateShaderResourceView(res, &srv, dst);

    // 【同时写一份 UAV】计算着色器要写同一个 SSBO（引擎用 StorageBuffer
    // 表示只读/读写两用），D3D12 的 UAV 与 SRV 是两种描述符，
    // 必须分别放在各自的区域里（根签名也各有表）。
    // 只有允许 UAV 的资源才能建 UAV（例如 UPLOAD 堆上的缓冲不行）。
    if (res->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.FirstElement = 0;
        uav.Buffer.NumElements  = nElems;
        uav.Buffer.StructureByteStride = stride;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        m_Device->CreateUnorderedAccessView(res, nullptr, &uav, SetWriteHandle(s, s.uavBase + binding));
    }
}

void D3D12Device::UpdateDescriptorSet(DescriptorSetHandle set, u32 binding, DescriptorType type,
                                      IRHITexture* texture, IRHISampler* sampler) {
    if (set == kInvalidSet || set > m_DescriptorSets.size()) return;
    if (!EnsureDescriptorHeaps()) return;

    auto& s = m_DescriptorSets[static_cast<size_t>(set - 1)];
    // 【采样器槽位 = 该 set 的采样器基址 + binding】采样器堆与 CBV_SRV_UAV 堆分开编号：
    // 不能叠加 baseSlot（CBV_SRV_UAV 堆有 65536 槽，叠加后会越过采样器堆上限）。
    // 每个 set 的采样器基址在 AllocateDescriptorSet 里独立分配，避免不同 set 互相覆盖。
    const u32 slot = s.samplerBase + binding;
    const bool samplerSlotOk = slot < m_BindlessSmpBase;   // 不得写进堆尾的 bindless 采样器区

    if (type == DescriptorType::Sampler) {
        if (auto* smp = static_cast<D3D12Sampler*>(sampler); smp && samplerSlotOk) {
            D3D12_CPU_DESCRIPTOR_HANDLE dh = m_SetSamplerHeap->GetCPUDescriptorHandleForHeapStart();
            dh.ptr += static_cast<SIZE_T>(slot) * m_SetSamplerSize;
            dh = smp->GetDescriptor();
            // 把采样器复制到描述符集的采样器堆槽位
            D3D12_CPU_DESCRIPTOR_HANDLE dstS = m_SetSamplerHeap->GetCPUDescriptorHandleForHeapStart();
            dstS.ptr += static_cast<SIZE_T>(slot) * m_SetSamplerSize;
            ID3D12DescriptorHeap* src = nullptr;
            (void)src;
            // 用 CopyDescriptorsSimple 从采样器自己的堆拷贝
            m_Device->CopyDescriptorsSimple(1, dstS, smp->GetDescriptor(),
                                            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        }
        return;
    }

    if (!texture) return;
    auto* tex = static_cast<D3D12Texture*>(texture);
    // 记下该 binding 的纹理：绘制/派发前由命令列表转到着色器可读（或 UAV）状态
    m_DescriptorSets[static_cast<size_t>(set - 1)].textures[binding] = tex->GetResource();
    m_DescriptorSets[static_cast<size_t>(set - 1)].subresources.erase(binding);   // 整资源绑定
    m_DescriptorSets[static_cast<size_t>(set - 1)].buffers.erase(binding);   // 同一 binding 改写为纹理
    // 描述符写在「该 set 的基址 + binding」：绑定时按 space 绑 set 的区段基址，
    // 写入与读取用同一套偏移才能对上。
    D3D12_CPU_DESCRIPTOR_HANDLE dst = SetWriteHandle(s, s.baseSlot + binding);
    // 整资源视图（立方体 / 3D / 2D 维度与深度格式映射见 BuildTextureViewDescs）
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    BuildTextureViewDescs(tex->GetResource()->GetDesc(), 0, 0, false, srv, uav);
    m_Device->CreateShaderResourceView(tex->GetResource(), &srv, dst);
    CheckDeviceAlive(m_Device.Get(), "CreateShaderResourceView(texture)");

    // StorageImage（RWTexture）需要一个 UAV 视图
    if (type == DescriptorType::StorageImage) {
        m_Device->CreateUnorderedAccessView(tex->GetResource(), nullptr, &uav,
                                            SetWriteHandle(s, s.uavBase + binding));
    }

    // 同时把采样器写到同一槽位（CombinedImageSampler 语义）
    if (auto* smp = static_cast<D3D12Sampler*>(sampler); smp && samplerSlotOk) {
        D3D12_CPU_DESCRIPTOR_HANDLE dstS = m_SetSamplerHeap->GetCPUDescriptorHandleForHeapStart();
        dstS.ptr += static_cast<SIZE_T>(slot) * m_SetSamplerSize;
        m_Device->CopyDescriptorsSimple(1, dstS, smp->GetDescriptor(),
                                        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        CheckDeviceAlive(m_Device.Get(), "CopyDescriptorsSimple(sampler)");
    }
}

void D3D12Device::UpdateDescriptorSet(DescriptorSetHandle set, u32 binding, DescriptorType type,
                                      IRHITexture** textures, IRHISampler** samplers, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        UpdateDescriptorSet(set, binding + i, type,
                            textures ? textures[i] : nullptr,
                            samplers ? samplers[i] : nullptr);
    }
}

void D3D12Device::UpdateDescriptorSet(DescriptorSetHandle set, u32 binding, DescriptorType type,
                                      IRHIBuffer** buffers, u32 count) {
    if (type == DescriptorType::Sampler) {
        // 采样器数组（引擎用 buffers=nullptr, samplers=... 之外的形态很少见）
        return;
    }
    for (u32 i = 0; i < count; ++i) {
        UpdateDescriptorSet(set, binding + i, type, buffers ? buffers[i] : nullptr);
    }
}

void D3D12Device::UpdateDescriptorSet(DescriptorSetHandle, u32, DescriptorType, IRHIAccelerationStructure*) {}
// 【关键】引擎用「图像视图句柄」更新描述符（例如把 HDR 纹理绑到 set=0/bind=0
// 的 u_HDRTexture_0）。此前这里是空实现，导致该槽永远是空描述符 ——
// 色调映射因此采样到空纹理，输出全黑（RenderDoc 实测 d.resource = ResourceId::0）。
void D3D12Device::UpdateDescriptorSetWithImageView(DescriptorSetHandle set, u32 binding,
                                                   DescriptorType type, void* imageView) {
    if (!m_Device || !imageView) return;
    // 引擎给的是「视图句柄」：反查它对应的资源与子资源范围（逐 mip / 逐层视图只覆盖一个子资源）
    const ViewInfo* vi = FindViewInfo(reinterpret_cast<UINT64>(imageView));
    if (!vi || !vi->res) return;
    ID3D12Resource* res = vi->res;
    const D3D12_RESOURCE_DESC rd = res->GetDesc();
    if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D) return;

    // 目标槽位：与 UpdateDescriptorSet(texture) 相同的布局（set 的 baseSlot / uavBase + binding）
    if (set == kInvalidSet || set > m_DescriptorSets.size()) return;
    if (!EnsureDescriptorHeaps()) return;
    auto& s = m_DescriptorSets[static_cast<size_t>(set - 1)];
    s.textures[binding] = res;   // 同上：供绘制前补状态转换
    s.buffers.erase(binding);
    // 逐 mip / 逐层视图：记下子资源下标（D3D12CalcSubresource：mip + 层 × mip 数；3D 纹理只有 mip 一维）
    if (vi->subresource) {
        const bool is3D = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        s.subresources[binding] = vi->mip + (is3D ? 0u : vi->layer) * rd.MipLevels;
    } else {
        s.subresources.erase(binding);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC  sd{};
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    BuildTextureViewDescs(rd, vi->mip, vi->layer, vi->subresource, sd, ud);
    const D3D12_CPU_DESCRIPTOR_HANDLE dst = SetWriteHandle(s, s.baseSlot + binding);
    if (!dst.ptr) return;
    m_Device->CreateShaderResourceView(res, &sd, dst);
    // StorageImage（RWTexture）由着色器的 u 寄存器读写：必须在 UAV 区写一个 UAV。
    // 此前这里只写 SRV、忽略了 type ⇒ compute 写入目标（Lumen 图集、Hi-Z 逐级）全部无效
    if (type == DescriptorType::StorageImage && (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
        m_Device->CreateUnorderedAccessView(res, nullptr, &ud, SetWriteHandle(s, s.uavBase + binding));
    }
}

} // namespace he::rhi::d3d12

#endif // _WIN32
