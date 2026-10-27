// ============================================================================
//  D3D12SwapChain.cpp
// ============================================================================
#include "D3D12SwapChain.h"
#include "D3D12Device.h"
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
    m_Format = desc.format;
    // FLIP 模型要求至少 2 个缓冲
    m_BufferCount = (desc.bufferCount >= 2) ? desc.bufferCount : 2;

    DXGI_FORMAT fmt = ToDxgiFormat(desc.format);
    if (fmt == DXGI_FORMAT_UNKNOWN) fmt = DXGI_FORMAT_B8G8R8A8_UNORM;

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
    CreateBuffers();
    HE_CORE_INFO("D3D12 交换链已重建 {}x{}", width, height);
}

bool D3D12SwapChain::AcquireNextImage() {
    if (!m_SwapChain) return false;
    m_Index = m_SwapChain->GetCurrentBackBufferIndex();
    return true;
}

void D3D12SwapChain::Present(bool vsync) {
    { static int s=0; if (s<4) { ++s; HE_CORE_INFO("[步骤] Present #{}", s); } }
    if (!m_SwapChain) return;

    // 帧计数 & 截图（截图必须在 Present 之前，见 CaptureIfRequested 的注释）
    ++m_FrameCount;
    CaptureIfRequested();

    // 帧计数日志：确认 D3D12 后端在真正出帧（每 60 帧打一次，避免刷屏）
    static u64 s_frames = 0;
    ++s_frames;
    if (s_frames % 60 == 1) {
        HE_CORE_INFO("[D3D12] 已呈现 {} 帧（索引 {}）", s_frames, m_Index);
    }
    const UINT flags = vsync ? 0u : DXGI_PRESENT_ALLOW_TEARING;
    // 【Present 前必须切回 PRESENT】否则下一次以 RTV 使用同一缓冲会被判非法
    if (m_BackBufferState != D3D12_RESOURCE_STATE_PRESENT) {
        ID3D12Resource* bb = GetCurrentBackBufferResource();
        ID3D12CommandQueue* q = m_Device ? m_Device->GetQueue() : nullptr;
        if (bb && q && m_Device->GetDevice()) {
            ComPtr<ID3D12CommandAllocator>    a;
            ComPtr<ID3D12GraphicsCommandList> l;
            if (SUCCEEDED(m_Device->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))) &&
                SUCCEEDED(m_Device->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&l)))) {
                D3D12_RESOURCE_BARRIER tb{};
                tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                tb.Transition.pResource   = bb;
                tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                tb.Transition.StateBefore = m_BackBufferState;
                tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
                l->ResourceBarrier(1, &tb);
                l->Close();
                ID3D12CommandList* lists[1] = { l.Get() };
                q->ExecuteCommandLists(1, lists);
            }
        }
        m_BackBufferState = D3D12_RESOURCE_STATE_PRESENT;
    }

    if (std::getenv("HE_SCAN_ALL") && m_Device) {
        static int s_n = 0;
        if (++s_n == 40) m_Device->ScanAllHdrTargets();   // 只扫一次，避免太慢
    }
    if (std::getenv("HE_STATS_DRAW") && m_Device) {
        static int s_n = 0;
        if (++s_n == 60) m_Device->PassStatsRead("第60帧的全部索引绘制");   // 只读一次
    }
    if (m_Device) m_Device->FlushRetired();   // 每帧回收上一帧延迟释放的资源
    const HRESULT hr = m_SwapChain->Present(vsync ? 1u : 0u, flags);
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
    const u32 idx = m_SwapChain->GetCurrentBackBufferIndex();
    if (idx >= m_Buffers.size()) return nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(idx) * m_RtvSize;
    return reinterpret_cast<void*>(h.ptr);
}

ID3D12Resource* D3D12SwapChain::GetCurrentBackBufferResource() const {
    if (!m_SwapChain) return nullptr;
    const u32 idx = m_SwapChain->GetCurrentBackBufferIndex();
    return idx < m_Buffers.size() ? m_Buffers[idx].Get() : nullptr;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12SwapChain::GetCurrentRtv() const {
    // 【必须用实时索引】DXGI 的 FLIP_DISCARD 交换链在每次 Present 后轮换后缓冲。
    // 引擎的 FramePresenter::BeginFrame() 会调 AcquireNextImage()，但**样例不一定
    // 都调用**（01.Triangle 就没有）⇒ 缓存的 m_Index 会过期，
    // 结果是「画到缓冲 0、呈现缓冲 1」⇒ 屏幕全黑。
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    if (!m_SwapChain || !m_RtvHeap) return h;
    const u32 idx = m_SwapChain->GetCurrentBackBufferIndex();
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
// ============================================================================
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")

namespace {

/// 把 RGBA8 像素写成 PNG（用 WIC，无第三方依赖）
bool SaveRgbaToPng(const wchar_t* path, const unsigned char* rgba, u32 w, u32 h) {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        HE_CORE_ERROR("[截图] 无法创建 WIC 工厂");
        return false;
    }
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return false;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr))) return false;
    if (FAILED(frame->Initialize(nullptr))) return false;
    if (FAILED(frame->SetSize(w, h))) return false;

    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(frame->SetPixelFormat(&fmt))) return false;

    const UINT stride = w * 4;
    const UINT size   = stride * h;
    if (FAILED(frame->WritePixels(h, stride, size, const_cast<BYTE*>(rgba)))) return false;
    if (FAILED(frame->Commit())) return false;
    if (FAILED(encoder->Commit())) return false;
    return true;
}

} // namespace

void D3D12SwapChain::CaptureIfRequested() {
    if (!m_CaptureInit) {
        m_CaptureInit = true;
        if (const char* v = std::getenv("HE_CAPTURE_FRAME")) m_CaptureAt = std::atoi(v);
        HE_CORE_INFO("[D3D12 截图] HE_CAPTURE_FRAME={} (0/-1 表示不截图)", m_CaptureAt);
    }
    if (m_CaptureAt <= 0 || static_cast<int>(m_FrameCount) != m_CaptureAt) return;

    ID3D12Device* dev = m_Device ? m_Device->GetDevice() : nullptr;
    const u32 liveIdx = m_SwapChain ? m_SwapChain->GetCurrentBackBufferIndex() : m_Index;
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
    b.Transition.StateBefore = m_BackBufferState;    // 真实状态（PRESENT 或 RENDER_TARGET）
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
    b.Transition.StateAfter  = m_BackBufferState;    // 恢复
    list->ResourceBarrier(1, &b);
    list->Close();

    {   // 实验：先把回读缓冲填 0x80，用来判断拷贝是否真的生效
        void* fill = nullptr; D3D12_RANGE r0{0,0};
        if (SUCCEEDED(readback->Map(0, &r0, &fill)) && fill) { memset(fill, 0x80, static_cast<size_t>(totalSize)); readback->Unmap(0, nullptr); }
    }
    ID3D12CommandList* lists[1] = { list.Get() };
    {   // 实验：读回前 8 字节，判断是 0x80（拷贝没生效）还是 0（源是黑的）
        void* chk = nullptr; D3D12_RANGE r1{0,0};
        if (SUCCEEDED(readback->Map(0, &r1, &chk)) && chk) {
            const unsigned char* b = static_cast<const unsigned char*>(chk);
            HE_CORE_ERROR("[CAP] first bytes = {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}",
                          b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7]);
            readback->Unmap(0, nullptr);
        }
    }
    queue->ExecuteCommandLists(1, lists);
    // 【必须等拷贝真正完成】此前只在自建围栏上等，若围栏/事件异常就会在
    // GPU 还没执行拷贝时就去 Map，读到的仍是填充值（实测 0x80 原封不动）。
    // 这里直接等引擎队列空闲，保证拷贝已落地。
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
    for (u32 y = 0; y < h; ++y) {
        const unsigned char* s = base + static_cast<size_t>(y) * rowPitch;
        unsigned char* d = rgba.data() + static_cast<size_t>(y) * w * 4;
        for (u32 x = 0; x < w; ++x) {
            d[x*4+0] = s[x*4+2];   // B → R
            d[x*4+1] = s[x*4+1];   // G
            d[x*4+2] = s[x*4+0];   // R → B
            d[x*4+3] = 255;
        }
    }
    readback->Unmap(0, nullptr);

    wchar_t path[128];
    swprintf_s(path, L"capture_frame%llu.png", static_cast<unsigned long long>(m_FrameCount));
    const bool ok = SaveRgbaToPng(path, rgba.data(), w, h);
    HE_CORE_INFO("[D3D12 截图] 第 {} 帧 {}x{} 保存: {}", m_FrameCount, w, h, ok ? "成功" : "失败");
}

} // namespace he::rhi::d3d12

#endif // _WIN32
