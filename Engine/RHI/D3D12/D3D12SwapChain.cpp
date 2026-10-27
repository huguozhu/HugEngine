// ============================================================================
//  D3D12SwapChain.cpp
// ============================================================================
#include "D3D12SwapChain.h"
#include "D3D12Device.h"
#include "D3D12Converters.h"
#include <cstdio>

#if defined(_WIN32)

namespace he::rhi::d3d12 {

D3D12SwapChain::~D3D12SwapChain() = default;

bool D3D12SwapChain::Initialize(D3D12Device* device, const SwapChainDesc& desc) {
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
    if (std::getenv("HE_D3D12_TRACE")) { std::printf("[TRACE] ...\n"); std::fflush(stdout); }
    if (!m_SwapChain) return;

    // 帧计数：确认 D3D12 后端在真正出帧（每 60 帧打一次，避免刷屏）
    static u64 s_frames = 0;
    ++s_frames;
    if (s_frames % 60 == 1) {
        HE_CORE_INFO("[D3D12] 已呈现 {} 帧（索引 {}）", s_frames, m_Index);
    }
    const UINT flags = vsync ? 0u : DXGI_PRESENT_ALLOW_TEARING;
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
    if (m_Index >= m_Buffers.size()) return nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(m_Index) * m_RtvSize;
    return reinterpret_cast<void*>(h.ptr);
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12SwapChain::GetCurrentRtv() const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(m_Index) * m_RtvSize;
    return h;
}

} // namespace he::rhi::d3d12

#endif // _WIN32
