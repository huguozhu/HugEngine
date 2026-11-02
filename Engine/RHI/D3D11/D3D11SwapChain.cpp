// ============================================================================
//  D3D11SwapChain.cpp —— D3D11 交换链（说明见头文件）
// ============================================================================
#include "D3D11SwapChain.h"
#include "D3D11Converters.h"
#include "D3D11Device.h"
#include "D3D11Resources.h"

#include <wincodec.h>   // 截图：WIC 写 PNG（无第三方依赖，与 D3D12 后端相同做法）
#include <cstdio>
#include <string>
#include <vector>

#include "RHI/ScreenCapture.h"   // 默认截图路径（与 Vulkan / D3D12 同一口径）

#pragma comment(lib, "windowscodecs.lib")

namespace he::rhi::d3d11 {

/// 交换链缓冲格式：FLIP 模型只支持 RGBA8 / BGRA8 / RGBA16F / RGB10A2 的 UNORM 形式，
/// sRGB 由 RTV 表达（GetTextureFormats 对 *_SRGB 给出 sRGB 的 RTV 格式）
static DXGI_FORMAT SwapChainBufferFormat(Format f) {
    switch (f) {
        case Format::RGBA8_SRGB:               return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::BGRA8_UNORM:
        case Format::BGRA8_SRGB:               return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Format::RGBA16_FLOAT:             return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case Format::A2B10G10R10_UNORM_PACK32: return DXGI_FORMAT_R10G10B10A2_UNORM;
        default:                               return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

D3D11SwapChain::~D3D11SwapChain() {
    m_BackBuffer.reset();
}

bool D3D11SwapChain::Initialize(D3D11Device* device, const SwapChainDesc& desc) {
    m_Device      = device;
    m_Width       = desc.width;
    m_Height      = desc.height;
    m_Format      = desc.format;
    m_DxgiFormat  = SwapChainBufferFormat(desc.format);
    m_BufferCount = desc.bufferCount >= 2 ? desc.bufferCount : 2u;   // FLIP 模型至少 2 个缓冲

    // 撕裂（关闭垂直同步时不等刷新）：需要 DXGI 1.5 且系统支持
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(device->GetFactory()->QueryInterface(IID_PPV_ARGS(&factory5)))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) {
            m_Tearing = (allow == TRUE);
        }
    }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width       = desc.width;
    sd.Height      = desc.height;
    sd.Format      = m_DxgiFormat;
    sd.SampleDesc  = { 1, 0 };
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = m_BufferCount;
    sd.Scaling     = DXGI_SCALING_STRETCH;
    sd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode   = DXGI_ALPHA_MODE_UNSPECIFIED;
    sd.Flags       = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | (m_Tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);

    HWND hwnd = static_cast<HWND>(desc.windowHandle);
    if (!HE_D3D11_CHECK(device->GetFactory()->CreateSwapChainForHwnd(device->GetDevice(), hwnd, &sd,
                                                                     nullptr, nullptr, &m_SwapChain),
                        "CreateSwapChainForHwnd")) {
        return false;
    }
    device->GetFactory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);   // Alt+Enter 由引擎自己管
    if (!CreateBackBuffer()) return false;
    HE_CORE_INFO("[D3D11] 交换链已创建 {}x{}（{} 个缓冲，格式 {}，撕裂={}）", m_Width, m_Height,
                 m_BufferCount, static_cast<u32>(m_DxgiFormat), m_Tearing ? "支持" : "不支持");
    return true;
}

bool D3D11SwapChain::CreateBackBuffer() {
    ComPtr<ID3D11Texture2D> buffer;
    if (!HE_D3D11_CHECK(m_SwapChain->GetBuffer(0, IID_PPV_ARGS(&buffer)), "IDXGISwapChain::GetBuffer")) return false;
    m_BackBuffer = std::make_unique<D3D11Texture>();
    return m_BackBuffer->InitializeExternal(m_Device, buffer, m_Format);
}

void D3D11SwapChain::Resize(u32 width, u32 height) {
    if (!m_SwapChain || (width == m_Width && height == m_Height)) return;
    m_Width  = width;
    m_Height = height;
    if (width == 0 || height == 0) return;   // 最小化：等恢复尺寸后再重建
    // 模式 2：RHI 线程上可能还有引用旧后缓冲的命令未执行，先全部落地
    if (auto* router = m_Device->GetSubmitRouter()) router->FlushAndWait();
    // ResizeBuffers 要求后缓冲没有任何外部引用：先解绑上下文里的渲染目标并释放包装
    ID3D11DeviceContext4* ctx = m_Device->GetContext();
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    m_Device->GetImmediateState().owner = nullptr;   // 各命令列表下次改动上下文时重新应用自己的状态
    m_BackBuffer.reset();
    ctx->Flush();
    const UINT flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | (m_Tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
    if (!HE_D3D11_CHECK(m_SwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, flags), "ResizeBuffers")) return;
    CreateBackBuffer();
}

void* D3D11SwapChain::GetCurrentBackBufferView() const {
    return m_BackBuffer ? m_BackBuffer->GetNativeHandle() : nullptr;
}

ID3D11RenderTargetView* D3D11SwapChain::GetRtv() const {
    return m_BackBuffer ? m_BackBuffer->GetRtv(0, 0) : nullptr;
}

void D3D11SwapChain::Present(bool vsync) {
    // 模式 2：交给 RHI 线程，排在同一帧的提交之后（返回 false 表示路由尚未就绪，退回就地执行）
    if (auto* router = m_Device->GetSubmitRouter(); router && router->RoutePresent(this, 0, nullptr, vsync)) return;
    PresentNow(vsync);
}

void D3D11SwapChain::PresentNow(bool vsync) {
    if (!m_SwapChain) return;
    ++m_FrameCount;
    CaptureIfRequested();
    // 调试开关 HE_D3D11_NO_VSYNC=1：强制关闭垂直同步（测 CPU / GPU 吞吐时不受刷新率限制）
    static const bool s_NoVsync = (std::getenv("HE_D3D11_NO_VSYNC") != nullptr);
    if (s_NoVsync) vsync = false;
    // 关闭垂直同步时允许撕裂（窗口模式下不等刷新）
    const UINT flags = (!vsync && m_Tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    const HRESULT hr = m_SwapChain->Present(vsync ? 1u : 0u, flags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        HE_CORE_ERROR("[D3D11] Present 失败：设备丢失 reason=0x{:08x}",
                      static_cast<u32>(m_Device->GetDevice()->GetDeviceRemovedReason()));
    }
    m_Device->OnPresent();   // 时间戳的 disjoint 查询按帧轮转（M3.7）
    m_Device->DrainDebugMessages();
}

// ---------------------------------------------------------------------------
//  截图（HE_CAPTURE_FRAME=N）
// ---------------------------------------------------------------------------
namespace {

/// 把 RGBA8 像素写成 PNG（WIC）
bool SaveRgbaToPng(const wchar_t* path, const unsigned char* rgba, u32 w, u32 h) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // 已初始化过时返回 S_FALSE / RPC_E_CHANGED_MODE，均可继续
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(w, h))) return false;
    // SetPixelFormat 是输入输出参数：PNG 编码器不接受 RGBA 时会改成它支持的 BGRA，
    // 此时必须按 BGRA 交换 R / B 再写，否则截图红蓝互换
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(frame->SetPixelFormat(&fmt))) return false;
    std::vector<unsigned char> pixels(rgba, rgba + static_cast<size_t>(w) * h * 4);
    if (IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA)) {
        for (size_t i = 0; i < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
    } else if (!IsEqualGUID(fmt, GUID_WICPixelFormat32bppRGBA)) {
        return false;   // 编码器给出其它像素格式：不支持
    }
    if (FAILED(frame->WritePixels(h, w * 4, w * 4 * h, pixels.data()))) return false;
    return SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

} // namespace

void D3D11SwapChain::RequestCapture(u32 frameIndex, const char* path) {
    // 显式请求优先于环境变量：m_CaptureAt 置为非负之后 CaptureIfRequested 不再读 HE_CAPTURE_FRAME
    m_CaptureAt   = static_cast<int>(frameIndex);
    m_CapturePath = (path && *path) ? path : std::string();
    const std::string out = m_CapturePath.empty()
                          ? rhi::DefaultCapturePath(frameIndex) : m_CapturePath;
    HE_CORE_INFO("[D3D11 截图] 已按帧号请求截图：第 {} 帧 → {}", frameIndex, out);
}

void D3D11SwapChain::CaptureIfRequested() {
    if (m_CaptureAt < 0) {
        m_CaptureAt = 0;
        if (const char* v = std::getenv("HE_CAPTURE_FRAME")) m_CaptureAt = std::atoi(v);
    }
    if (m_CaptureAt <= 0 || static_cast<int>(m_FrameCount) != m_CaptureAt || !m_BackBuffer) return;
    if (m_DxgiFormat != DXGI_FORMAT_R8G8B8A8_UNORM && m_DxgiFormat != DXGI_FORMAT_B8G8R8A8_UNORM) {
        HE_CORE_WARN("[D3D11 截图] 只支持 8 位交换链格式，当前 {}", static_cast<u32>(m_DxgiFormat));
        return;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = m_Width; td.Height = m_Height; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = m_DxgiFormat;
    td.SampleDesc = { 1, 0 };
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(m_Device->GetDevice()->CreateTexture2D(&td, nullptr, &staging))) return;
    ID3D11DeviceContext4* ctx = m_Device->GetContext();
    ctx->CopyResource(staging.Get(), m_BackBuffer->GetResource());
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &ms))) return;
    std::vector<unsigned char> rgba(static_cast<size_t>(m_Width) * m_Height * 4);
    const bool bgra = (m_DxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM);
    for (u32 y = 0; y < m_Height; ++y) {
        const auto* src = static_cast<const unsigned char*>(ms.pData) + static_cast<size_t>(y) * ms.RowPitch;
        unsigned char* dst = rgba.data() + static_cast<size_t>(y) * m_Width * 4;
        for (u32 x = 0; x < m_Width; ++x) {
            dst[x * 4 + 0] = src[x * 4 + (bgra ? 2 : 0)];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + (bgra ? 0 : 2)];
            dst[x * 4 + 3] = 255;   // 后缓冲的 alpha 不参与显示，存成不透明便于查看
        }
    }
    ctx->Unmap(staging.Get(), 0);
    // 落盘路径：显式请求的路径优先，否则用与其它后端一致的默认路径（<exe 目录>/capture_frameN.png）
    const std::string out = m_CapturePath.empty()
                          ? rhi::DefaultCapturePath(static_cast<u64>(m_CaptureAt)) : m_CapturePath;
    // WIC 走宽字符路径：UTF-8 → UTF-16
    std::wstring wpath;
    if (const int n = MultiByteToWideChar(CP_UTF8, 0, out.c_str(), -1, nullptr, 0); n > 0) {
        wpath.resize(static_cast<size_t>(n) - 1);
        MultiByteToWideChar(CP_UTF8, 0, out.c_str(), -1, wpath.data(), n);
    }
    const bool ok = !wpath.empty() && SaveRgbaToPng(wpath.c_str(), rgba.data(), m_Width, m_Height);
    HE_CORE_INFO("[D3D11 截图] 第 {} 帧 → {}（{}）", m_CaptureAt, out, ok ? "成功" : "失败");
}

} // namespace he::rhi::d3d11
