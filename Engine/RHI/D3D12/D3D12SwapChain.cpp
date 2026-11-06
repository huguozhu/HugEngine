// ============================================================================
//  D3D12SwapChain.cpp
// ============================================================================
#include "D3D12SwapChain.h"
#include "D3D12Device.h"
#include "D3D12Resources.h"     // 【P10】DumpStorageBufferStats（Storage 缓冲堆统计）
#include "RHI/SubmitRouter.h"   // 模式 2：呈现交给提交路由
#include "RHI/ScreenCapture.h"  // P8：截图公共工具（环境变量 / 默认路径 / WIC 写 PNG）
#include "D3D12Converters.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

#if defined(_WIN32)

namespace he::rhi::d3d12 {

D3D12SwapChain::~D3D12SwapChain() = default;

bool D3D12SwapChain::Initialize(D3D12Device* device, const SwapChainDesc& desc) {
    if (device) device->SetSwapChainPtr(this);   // 供命令列表兜底取用

    if (!device || !device->GetFactory() || !device->GetQueue()) return false;
    m_Device = device;
    m_Width  = desc.width;
    m_Height = desc.height;
    m_Vsync  = desc.vsync;
    // FLIP 模型要求至少 2 个缓冲
    m_BufferCount = (desc.bufferCount >= 2) ? desc.bufferCount : 2;

    DXGI_FORMAT fmt = ToDxgiFormat(desc.format);
    // 【与 Vulkan 后端保持一致：后缓冲统一 BGRA8】
    //   · DXGI 的 FLIP 模型对 BGRA8 支持最广（R8G8B8A8 在部分驱动 / 呈现路径上会被拒绝）；
    //   · 引擎的后处理链按 BGRA8 写死（PostProcessChain 的 LDR 中间纹理、FXAA 的 PSO），
    //     样例的 BeginRenderPass 也传 BGRA8。若后缓冲是 R8G8B8A8，D3D12 会因
    //     「PSO 第 0 个渲染目标的格式与 RTV 不匹配」丢弃绘制（调试层 id=613，实测
    //     05.Sponza-PathTracing 每帧两条：ToneMap→LDR 与 FXAA→后缓冲各一条）。
    //   SwapChainDesc::format 的默认值是 RGBA8_UNORM，故这里把它当作「默认」改用 BGRA8。
    if (fmt == DXGI_FORMAT_UNKNOWN || fmt == DXGI_FORMAT_R8G8B8A8_UNORM)
        fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (desc.hdr) fmt = DXGI_FORMAT_R10G10B10A2_UNORM;   // HDR10 输出
    m_Format = (fmt == DXGI_FORMAT_R10G10B10A2_UNORM) ? Format::A2B10G10R10_UNORM_PACK32
                                                      : Format::BGRA8_UNORM;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width  = desc.width;
    sd.Height = desc.height;
    sd.Format = fmt;
    sd.Stereo = FALSE;
    sd.SampleDesc = { 1, 0 };
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = m_BufferCount;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    // 【P10】无垂直同步（vsync=false）时必须建带 ALLOW_TEARING 的交换链，否则
    //   `Present(0, DXGI_PRESENT_ALLOW_TEARING)` 每帧返回 DXGI_ERROR_INVALID_CALL
    //   （实测：HE_NO_VSYNC=1 时 04/06/07 的 D3D12 路径全部报错、帧时间测量失去意义）。
    //   只有适配器报告支持 tearing 时才加；不支持就保持旧行为（下面 Present 会退回 syncInterval=1）。
    {
        bool tearingSupported = false;
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(device->GetFactory()->QueryInterface(IID_PPV_ARGS(&factory5))) && factory5) {
            BOOL allow = FALSE;
            if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                         &allow, sizeof(allow)))) {
                tearingSupported = (allow != FALSE);
            }
        }
        m_TearingEnabled = tearingSupported && !desc.vsync;
        if (m_TearingEnabled) sd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        HE_CORE_INFO("[D3D12 交换链] tearing 支持={} 本次启用={}（vsync={}）",
                     tearingSupported ? 1 : 0, m_TearingEnabled ? 1 : 0, desc.vsync ? 1 : 0);
    }

    HWND hwnd = static_cast<HWND>(desc.windowHandle);
    ComPtr<IDXGISwapChain1> sc1;
    if (!HE_D3D12_CHECK(device->GetFactory()->CreateSwapChainForHwnd(
                            device->GetQueue(), hwnd, &sd, nullptr, nullptr, &sc1),
                        "CreateSwapChainForHwnd")) {
        return false;
    }
    // 禁止 DXGI 自己处理 Alt+Enter（引擎自己管）
    device->GetFactory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&m_SwapChain);
    if (!m_SwapChain) return false;

    m_Index = m_SwapChain->GetCurrentBackBufferIndex();
    m_RtvSize = device->GetRtvIncrement();

    if (!CreateBuffers()) return false;

    HE_CORE_INFO("D3D12 交换链已创建 {}x{} ({} buffers, fmt={})",
                 m_Width, m_Height, m_BufferCount, static_cast<u32>(fmt));
    return true;
}

bool D3D12SwapChain::CreateBuffers() {
    if (!m_Device || !m_SwapChain) return false;
    ID3D12Device* dev = m_Device->GetDevice();

    // RTV 堆：每个 back buffer 一个描述符
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = m_BufferCount;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (!HE_D3D12_CHECK(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_RtvHeap)),
                        "CreateDescriptorHeap(SwapChain RTV)")) {
        return false;
    }

    m_Buffers.resize(m_BufferCount);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (u32 i = 0; i < m_BufferCount; ++i) {
        if (!HE_D3D12_CHECK(m_SwapChain->GetBuffer(i, IID_PPV_ARGS(&m_Buffers[i])),
                            "SwapChain::GetBuffer")) {
            return false;
        }
        dev->CreateRenderTargetView(m_Buffers[i].Get(), nullptr, rtv);
        // 【T9：后缓冲也进设备的资源状态表】
        //  D3D12 的屏障要求 StateBefore 严格等于资源**实际**状态，而此前后缓冲有两套记录：
        //  交换链自己的 m_BackBufferState（只有 BeginRenderPass / Present 写），以及设备的
        //  状态表（BeginOffscreenPass 系经 FindViewResource 反查、找不到就跳过）。后缓冲的
        //  RTV 从没登记过，于是：
        //    · 若某帧只用 BeginOffscreenPass 写后缓冲，实际状态被改成 RENDER_TARGET 却无人记录
        //      （或干脆没做转换），截图与 Present 的屏障就会声明错状态（调试层 id=527，
        //      约 6 次运行出现 1 次）；
        //    · 两套记录还可能对不上。
        //  统一做法：后缓冲的每个 RTV/资源都登记进设备表，状态以设备表为唯一事实来源。
        //  初始状态是 PRESENT（等价于 COMMON，DXGI 的 FLIP 模型在首次 Present 前就是这个语义）。
        m_Device->RegisterViewResource(static_cast<UINT64>(rtv.ptr), m_Buffers[i].Get());
        m_Device->SetResourceState(m_Buffers[i].Get(), D3D12_RESOURCE_STATE_PRESENT);
        rtv.ptr += m_RtvSize;
    }
    return true;
}

void D3D12SwapChain::Resize(u32 width, u32 height) {
    if (!m_SwapChain || !m_Device) return;
    if (width == 0 || height == 0) return;
    if (width == m_Width && height == m_Height) return;

    // 重建前必须等 GPU 空闲，否则会在用中的资源上做 ResizeBuffers
    m_Device->WaitIdle();
    // 旧后缓冲的 RTV 将随堆一起重建：先把设备状态表里的登记与状态清掉，
    // 否则同一 CPU 句柄地址会被新资源复用，状态表里却还留着旧资源的状态记录
    if (m_RtvHeap) {
        for (u32 i = 0; i < m_Buffers.size(); ++i)
            m_Device->UnregisterViewResource(static_cast<UINT64>(GetRtv(i).ptr));
    }
    for (auto& b : m_Buffers) b.Reset();

    if (FAILED(m_SwapChain->ResizeBuffers(m_BufferCount, width, height,
                                          DXGI_FORMAT_UNKNOWN,
                                          DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH))) {
        HE_CORE_ERROR("D3D12SwapChain::ResizeBuffers 失败 {}x{}", width, height);
        return;
    }
    m_Width = width;
    m_Height = height;
    m_Index = m_SwapChain->GetCurrentBackBufferIndex();
    // 新后缓冲的初始状态是 PRESENT/COMMON（CreateBuffers 里会重新登记设备状态）
    m_BackBufferState = D3D12_RESOURCE_STATE_PRESENT;
    CreateBuffers();
    HE_CORE_INFO("D3D12 交换链已重建 {}x{}", width, height);
}

bool D3D12SwapChain::AcquireNextImage() {
    if (!m_SwapChain) return false;
    if (!m_PresentRouted) m_Index = m_SwapChain->GetCurrentBackBufferIndex();
    return true;
}

u32 D3D12SwapChain::RecordIndex() const {
    if (m_PresentRouted || !m_SwapChain) return m_Index;
    return m_SwapChain->GetCurrentBackBufferIndex();
}

// ---------------------------------------------------------------------------
//  Present（渲染线程）
//  模式 2 下真正的呈现在 RHI 线程上执行。渲染线程在这里记下本帧的后缓冲序号与状态，
//  并立刻把自己的记录推进到下一帧（序号 +1、状态回到 PRESENT），之后录制下一帧时不再依赖
//  RHI 线程何时呈现。D3D12 没有呈现信号量，状态借 RoutePresent 的 waitSemaphore 参数传过去。
// ---------------------------------------------------------------------------
void D3D12SwapChain::Present(bool vsync) {
    { static int s=0; if (s<4) { ++s; HE_CORE_INFO("[步骤] Present #{}", s); } }
    if (!m_SwapChain) return;
    if (m_Device) m_Device->FlushRetired();   // 回收延迟释放的资源（会 WaitIdle，只能在渲染线程做）
    const u32 index = RecordIndex();
    // 【T9：真实状态以设备的资源状态表为准】交换链的 m_BackBufferState 只是一份镜像
    //（命令流回放要用它，见 BeginRenderPass 的 hinted 路径）。此前后缓冲只在这里与
    // BeginRenderPass 里记录状态，一旦某帧经 BeginOffscreenPass 把后缓冲当附件写，
    // 实际状态变成 RENDER_TARGET 却无人登记 ⇒ 截图列表声明 PRESENT 与实际不符（id=527）。
    // 现在设备表在每次转换后都会被更新，Present 直接查它。
    ID3D12Resource* bb = (index < m_Buffers.size()) ? m_Buffers[index].Get() : nullptr;
    const D3D12_RESOURCE_STATES before =
        (m_Device && bb) ? m_Device->GetResourceState(bb) : m_BackBufferState;
    m_BackBufferState = D3D12_RESOURCE_STATE_PRESENT;
    // 渲染线程侧就登记成 PRESENT：本帧在 GPU 上的最后一步是"后缓冲 → PRESENT"，
    // 下一帧录制 BeginRenderPass 时读到的正是这个值
    if (m_Device && bb) m_Device->SetResourceState(bb, D3D12_RESOURCE_STATE_PRESENT);
    ISubmitRouter* router = m_Device ? m_Device->GetSubmitRouter() : nullptr;
    if (router && router->RoutePresent(this, index, reinterpret_cast<void*>(static_cast<uintptr_t>(before)), vsync)) {
        m_PresentRouted = true;
        m_Index = (index + 1) % m_BufferCount;   // 翻转模型按顺序轮换后缓冲
        return;
    }
    m_PresentRouted = false;
    PresentNow(index, before, vsync);
    m_Index = m_SwapChain->GetCurrentBackBufferIndex();
}

bool D3D12SwapChain::IsBackBuffer(const ID3D12Resource* res) const {
    if (!res) return false;
    for (const auto& b : m_Buffers) {
        if (b.Get() == res) return true;
    }
    return false;
}

void D3D12SwapChain::PresentNow(u32 index, D3D12_RESOURCE_STATES before, bool vsync) {
    if (!m_SwapChain) return;

    // 帧计数 & 截图（截图必须在 Present 之前，见 CaptureIfRequested 的注释）
    ++m_FrameCount;
    CaptureIfRequested(index, before);

    // 帧计数日志：确认 D3D12 后端在真正出帧（每 60 帧打一次，避免刷屏）
    static u64 s_frames = 0;
    ++s_frames;
    if (s_frames % 60 == 1) {
        HE_CORE_INFO("[D3D12] 已呈现 {} 帧（索引 {}）", s_frames, m_Index);
    }
    // 【P10】Storage 缓冲堆统计：第 60 帧打一次（此时样例的缓冲都已建好），之后每 600 帧一次
    if (s_frames == 60 || s_frames % 600 == 0) {
        DumpStorageBufferStats(s_frames == 60 ? "首帧" : "运行中");
    }
    // 【P10】只有交换链创建时带了 ALLOW_TEARING 才能用「syncInterval=0 + ALLOW_TEARING」；
    //   否则退回 syncInterval=1（否则 Present 每帧 DXGI_ERROR_INVALID_CALL）。
    const bool presentNoVsync = !vsync && m_TearingEnabled;
    const UINT flags = presentNoVsync ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    // 【Present 前必须切回 PRESENT】否则下一次以 RTV 使用同一缓冲会被判非法
    if (before != D3D12_RESOURCE_STATE_PRESENT) {
        ID3D12Resource* bb = index < m_Buffers.size() ? m_Buffers[index].Get() : nullptr;
        ID3D12CommandQueue* q = m_Device ? m_Device->GetQueue() : nullptr;
        ID3D12Device* dev = m_Device ? m_Device->GetDevice() : nullptr;
        // 首次使用时创建常驻的分配器 / 列表 / 围栏
        if (dev && !m_PresentList) {
            bool ok = true;
            for (auto& a : m_PresentAllocs)
                ok = ok && SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)));
            ok = ok && SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_PresentAllocs[0].Get(),
                                                        nullptr, IID_PPV_ARGS(&m_PresentList)));
            ok = ok && SUCCEEDED(m_PresentList->Close());
            if (ok) m_PresentList->SetName(L"HugEngine 呈现屏障列表");   // DRED / RenderDoc 里据此识别
            ok = ok && SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_PresentFence)));
            if (!ok) m_PresentList.Reset();
            if (!ok) HE_CORE_ERROR("[D3D12] 呈现屏障列表创建失败：后缓冲将停在 RENDER_TARGET（状态会与实际不符）");
        }
        if (bb && q && m_PresentList) {
            // 轮换槽位；该槽位的分配器必须等 GPU 执行完上一次的屏障后才能 Reset
            m_PresentSlot = (m_PresentSlot + 1) % kMaxFramesInFlight;
            if (m_PresentFence->GetCompletedValue() < m_PresentFenceValues[m_PresentSlot]) {
                if (HANDLE ev = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS)) {
                    m_PresentFence->SetEventOnCompletion(m_PresentFenceValues[m_PresentSlot], ev);
                    WaitForSingleObject(ev, 8000);
                    CloseHandle(ev);
                }
            }
            ID3D12CommandAllocator* a = m_PresentAllocs[m_PresentSlot].Get();
            ID3D12GraphicsCommandList* l = m_PresentList.Get();
            if (SUCCEEDED(a->Reset()) && SUCCEEDED(l->Reset(a, nullptr))) {
                D3D12_RESOURCE_BARRIER tb{};
                tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                tb.Transition.pResource   = bb;
                tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                tb.Transition.StateBefore = before;
                tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
                l->ResourceBarrier(1, &tb);
                l->Close();
                ID3D12CommandList* lists[1] = { l };
                q->ExecuteCommandLists(1, lists);
                m_PresentFenceValues[m_PresentSlot] = m_PresentNextValue;
                q->Signal(m_PresentFence.Get(), m_PresentNextValue++);
            } else {
                // 【T9】静默跳过会让"实际状态仍是 RENDER_TARGET"却无人记录，后面的屏障全部错位
                HE_CORE_ERROR("[D3D12] 呈现屏障的命令分配器/列表重置失败：后缓冲未转回 PRESENT");
            }
        } else if (!bb || !q) {
            HE_CORE_ERROR("[D3D12] 呈现屏障缺少后缓冲或队列：后缓冲未转回 PRESENT");
        }
    }

    // 诊断开关每帧都要判断：只在首次读一次环境变量
    static const bool s_ScanAll  = (std::getenv("HE_SCAN_ALL") != nullptr);
    static const bool s_StatsDraw = (std::getenv("HE_STATS_DRAW") != nullptr);
    if (s_ScanAll && m_Device) {
        static int s_n = 0;
        if (++s_n == 40) m_Device->ScanAllHdrTargets();   // 只扫一次，避免太慢
    }
    if (s_StatsDraw && m_Device) {
        static int s_n = 0;
        if (++s_n == 60) m_Device->PassStatsRead("第60帧的全部索引绘制");   // 只读一次
    }
    const HRESULT hr = m_SwapChain->Present(presentNoVsync ? 0u : 1u, flags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        m_Device->ReportDeviceRemoved("SwapChain::Present");
        return;
    }
    if (FAILED(hr)) {
        HE_CORE_ERROR("[D3D12] Present 失败 hr=0x{:08x} {}", static_cast<u32>(hr), HrName(hr));
    }
}

void* D3D12SwapChain::GetCurrentBackBufferView() const {
    if (!m_SwapChain || !m_RtvHeap) return nullptr;
    const u32 idx = RecordIndex();
    if (idx >= m_Buffers.size()) return nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(idx) * m_RtvSize;
    return reinterpret_cast<void*>(h.ptr);
}

ID3D12Resource* D3D12SwapChain::GetCurrentBackBufferResource() const {
    if (!m_SwapChain) return nullptr;
    const u32 idx = RecordIndex();
    return idx < m_Buffers.size() ? m_Buffers[idx].Get() : nullptr;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12SwapChain::GetRtv(u32 index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h{};
    if (!m_RtvHeap || index >= m_Buffers.size()) return h;
    h = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) * m_RtvSize;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12SwapChain::GetCurrentRtv() const {
    // 【必须用实时索引】DXGI 的 FLIP_DISCARD 交换链在每次 Present 后轮换后缓冲。
    // 引擎的 FramePresenter::BeginFrame() 会调 AcquireNextImage()，但**样例不一定
    // 都调用**（01.Triangle 就没有）⇒ 缓存的 m_Index 会过期，
    // 结果是「画到缓冲 0、呈现缓冲 1」⇒ 屏幕全黑。
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    if (!m_SwapChain || !m_RtvHeap) return h;
    const u32 idx = RecordIndex();   // 模式 2 下为渲染线程推进的序号
    h.ptr += static_cast<SIZE_T>(idx) * m_RtvSize;
    return h;
}


// ============================================================================
//  截图支持（调试用）
//
//  用法：set HE_CAPTURE_FRAME=3   → 第 3 帧在 Present 之前把后缓冲存成
//        capture_frame3.png
//
//  【为什么必须在 Present 之前】FLIP_DISCARD 交换链在 Present 之后内容未定义，
//  之后读到的永远是空缓冲（在 D3D11 后端上踩过这个坑）。
//
//  【P8】PNG 写出与环境变量解析已抽到 RHI 层（RHI/ScreenCapture.h），
//  Vulkan 后端走同一份代码 ⇒ 两后端的文件名与像素格式一致，便于逐像素对比。
// ============================================================================
void D3D12SwapChain::RequestCapture(u32 frameIndex, const char* path) {
    // 显式请求优先于环境变量：置 m_CaptureInit 后 CaptureIfRequested 不再读 env
    m_CaptureInit = true;
    m_CaptureAt   = static_cast<int>(frameIndex);
    m_CapturePath = (path && *path) ? path : std::string();
    const std::string out = m_CapturePath.empty()
                          ? rhi::DefaultCapturePath(frameIndex) : m_CapturePath;
    HE_CORE_INFO("[D3D12 截图] 已按帧号请求截图：第 {} 帧 → {}", frameIndex, out);
}

void D3D12SwapChain::CaptureIfRequested(u32 index, D3D12_RESOURCE_STATES state) {
    if (!m_CaptureInit) {
        m_CaptureInit = true;
        m_CaptureAt = rhi::CaptureFrameFromEnv();
        HE_CORE_INFO("[D3D12 截图] HE_CAPTURE_FRAME={} (0/-1 表示不截图)", m_CaptureAt);
    }
    if (m_CaptureAt <= 0 || static_cast<int>(m_FrameCount) != m_CaptureAt) return;

    ID3D12Device* dev = m_Device ? m_Device->GetDevice() : nullptr;
    const u32 liveIdx = index;   // 本次要呈现的后缓冲（模式 2 下在 RHI 线程上执行，序号由渲染线程登记）
    if (!dev || liveIdx >= m_Buffers.size() || !m_Buffers[liveIdx]) return;

    // 【做法】后缓冲 → 回读**缓冲**（不是回读纹理），用 CopyTextureRegion
    // 带 PlacedFootprint 表达行间距。回读纹理在部分驱动上 CreateCommittedResource
    // 会返回 E_INVALIDARG（实测就是这里失败），缓冲形式则稳定。
    const D3D12_RESOURCE_DESC bd = m_Buffers[liveIdx]->GetDesc();
    const u32 w = static_cast<u32>(bd.Width);
    const u32 h = static_cast<u32>(bd.Height);
    const u32 rowPitch = (w * 4 + 255u) & ~255u;     // D3D12 要求 256 对齐
    const u64 totalSize = static_cast<u64>(rowPitch) * h;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask  = 1;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = totalSize;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc       = { 1, 0 };
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags            = D3D12_RESOURCE_FLAG_NONE;

    ComPtr<ID3D12Resource> readback;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&readback));
    if (FAILED(hr)) {
        HE_CORE_ERROR("[D3D12 截图] 回读缓冲创建失败 hr=0x{:08x}", static_cast<u32>(hr));
        return;
    }

    // 【必须用引擎自己的队列】另建队列会把「后缓冲的状态转换」和「引擎的绘制」
    // 放到两个队列上并发执行，D3D12 会直接判定资源被跨队列争用 ⇒ DEVICE_HUNG
    //（实测另建队列后设备每秒丢一次）。用同一个队列则天然 FIFO 有序。
    ID3D12CommandQueue* queue = m_Device ? m_Device->GetQueue() : nullptr;
    if (!queue) return;

    ComPtr<ID3D12CommandAllocator>    alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                      IID_PPV_ARGS(&list)))) {
        HE_CORE_ERROR("[D3D12 截图] 命令对象创建失败");
        return;
    }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = m_Buffers[liveIdx].Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = state;    // 真实状态（PRESENT 或 RENDER_TARGET）
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource       = readback.Get();
    dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format   = bd.Format;
    dst.PlacedFootprint.Footprint.Width    = w;
    dst.PlacedFootprint.Footprint.Height   = h;
    dst.PlacedFootprint.Footprint.Depth    = 1;
    dst.PlacedFootprint.Footprint.RowPitch = rowPitch;
    dst.PlacedFootprint.Offset             = 0;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource        = m_Buffers[liveIdx].Get();
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter  = state;    // 恢复
    list->ResourceBarrier(1, &b);
    list->Close();

    ID3D12CommandList* lists[1] = { list.Get() };
    queue->ExecuteCommandLists(1, lists);
    // 【必须等拷贝真正完成】此前只在自建围栏上等，若围栏/事件异常就会在
    // GPU 还没执行拷贝时就去 Map。这里直接等引擎队列空闲，保证拷贝已落地。
    if (m_Device) m_Device->WaitIdle();

    ComPtr<ID3D12Fence> fence;
    if (SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        HANDLE ev = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        if (ev) {
            queue->Signal(fence.Get(), 1);
            fence->SetEventOnCompletion(1, ev);
            WaitForSingleObject(ev, 5000);
            CloseHandle(ev);
        }
    }

    D3D12_RANGE range{ 0, 0 };
    void* mapped = nullptr;
    if (FAILED(readback->Map(0, &range, &mapped)) || !mapped) {
        HE_CORE_ERROR("[D3D12 截图] 映射回读缓冲失败");
        return;
    }

    std::vector<unsigned char> rgba(static_cast<size_t>(w) * h * 4);
    const unsigned char* base = static_cast<const unsigned char*>(mapped);
    // 【通道顺序】WIC 的 GUID_WICPixelFormat32bppRGBA 编码器实际按 BGRA 解读数据后写 PNG
    //（实测：BGRA 后缓冲 + 这里再交换 R/B ⇒ PNG 整体反色，蓝色横幅变成橙色）。
    // 因此只在后缓冲确实是 R8G8B8A8 时交换；后缓冲是 BGRA8（Initialize 的默认选择）时
    // 原样交给 WIC，PNG 才是正确颜色。
    const bool swapRB = (bd.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
    for (u32 y = 0; y < h; ++y) {
        const unsigned char* s = base + static_cast<size_t>(y) * rowPitch;
        unsigned char* d = rgba.data() + static_cast<size_t>(y) * w * 4;
        for (u32 x = 0; x < w; ++x) {
            d[x*4+0] = swapRB ? s[x*4+2] : s[x*4+0];
            d[x*4+1] = s[x*4+1];
            d[x*4+2] = swapRB ? s[x*4+0] : s[x*4+2];
            d[x*4+3] = 255;
        }
    }
    readback->Unmap(0, nullptr);

    // 【P8】路径与 PNG 写出走 RHI 公共工具：默认 `<exe 目录>/capture_frameN.png`，
    // 与 Vulkan 后端的默认路径/像素格式完全一致，便于逐像素对比。
    const std::string path = m_CapturePath.empty()
                           ? rhi::DefaultCapturePath(m_FrameCount) : m_CapturePath;
    const bool ok = rhi::SaveRgbaToPng(path.c_str(), rgba.data(), w, h);
    HE_CORE_INFO("[D3D12 截图] 第 {} 帧 {}x{} 保存{}: {}", m_FrameCount, w, h,
                 ok ? "成功" : "失败", path);
}

} // namespace he::rhi::d3d12

#endif // _WIN32
