// ============================================================
// RHI/ScreenCapture.cpp — 按帧截图公共工具实现
//
// 依赖：Windows Imaging Component（WIC，windowscodecs.lib）
//       Vulkan / D3D12 后端都只跑在 Windows 上，故这里不提供跨平台回退；
//       非 Windows 平台下 SaveRgbaToPng 返回 false 并打印一次告警。
// ============================================================
#include "RHI/ScreenCapture.h"

#include "Core/Log.h"

#if defined(_WIN32)
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>   // ComPtr
#pragma comment(lib, "windowscodecs.lib")
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace he::rhi {

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;
#endif

#if defined(_WIN32)

namespace {

/// UTF-8 → UTF-16（Windows API 只吃宽字符路径）。失败返回空串。
std::wstring Utf8ToWide(const char* utf8) {
    if (!utf8 || !*utf8) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), n) != n) return {};
    return w;
}

/// UTF-16 → UTF-8（默认截图路径用）。失败返回空串。
std::string WideToUtf8(const wchar_t* wide) {
    if (!wide) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    if (::WideCharToMultiByte(CP_UTF8, 0, wide, -1, s.data(), n, nullptr, nullptr) != n) return {};
    return s;
}

} // namespace

// ============================================================
// CaptureFrameFromEnv — 读一次环境变量并缓存
// ============================================================
int CaptureFrameFromEnv() {
    static const int s_Frame = [] {
        if (const char* v = std::getenv("HE_CAPTURE_FRAME")) return std::atoi(v);
        return 0;
    }();
    return s_Frame;
}

// ============================================================
// DefaultCapturePath — <exe 目录>/capture_frameN.png
// ============================================================
std::string DefaultCapturePath(u64 frameIndex) {
    char name[64] = {};
    std::snprintf(name, sizeof(name), "capture_frame%llu.png",
                  static_cast<unsigned long long>(frameIndex));

    wchar_t exePath[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return name;   // 取不到 exe 路径：退回相对文件名（= CWD）

    // 去掉可执行文件名，保留目录（含结尾反斜杠）
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash) *(slash + 1) = L'\0';
    const std::wstring full = std::wstring(exePath) + Utf8ToWide(name);
    const std::string utf8 = WideToUtf8(full.c_str());
    return utf8.empty() ? name : utf8;
}

// ============================================================
// SaveRgbaToPng — WIC 写 PNG
//
// 【通道顺序】沿用 D3D12 原实现的实测结论：`GUID_WICPixelFormat32bppRGBA` 的编码器
// 在本机按 BGRA 解读数据（BGRA 后缓冲原样传入得到正确颜色，再交换 R/B 反而整体反色）。
// 因此调用方只需把后缓冲按"内存原样"（BGRA8）拷进 rgba 缓冲，本函数不做交换。
// ============================================================
bool SaveRgbaToPng(const char* path, const unsigned char* rgba, u32 width, u32 height) {
    if (!path || !*path || !rgba || width == 0 || height == 0) return false;

    // WIC 需要 COM 初始化。引擎此前没有任何 CoInitialize 调用（D3D12 截图为实测可用），
    // 这里做一次"尽力而为"的初始化：已初始化时返回 S_FALSE / RPC_E_CHANGED_MODE，
    // 两种情况都不影响后续 CoCreateInstance，故忽略返回值。
    static const bool s_ComTried = [] {
        ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        return true;
    }();
    (void)s_ComTried;

    const std::wstring wpath = Utf8ToWide(path);
    if (wpath.empty()) {
        HE_CORE_ERROR("[截图] 输出路径不是合法 UTF-8：{}", path);
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        HE_CORE_ERROR("[截图] 无法创建 WIC 工厂");
        return false;
    }
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE))) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return false;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr))) return false;
    if (FAILED(frame->Initialize(nullptr))) return false;
    if (FAILED(frame->SetSize(width, height))) return false;

    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(frame->SetPixelFormat(&fmt))) return false;

    const UINT stride = width * 4;
    const UINT size   = stride * height;
    if (FAILED(frame->WritePixels(height, stride, size, const_cast<BYTE*>(rgba)))) return false;
    if (FAILED(frame->Commit())) return false;
    if (FAILED(encoder->Commit())) return false;
    return true;
}

#else  // !_WIN32

int CaptureFrameFromEnv() {
    if (const char* v = std::getenv("HE_CAPTURE_FRAME")) return std::atoi(v);
    return 0;
}

std::string DefaultCapturePath(u64 frameIndex) {
    char name[64] = {};
    std::snprintf(name, sizeof(name), "capture_frame%llu.png",
                  static_cast<unsigned long long>(frameIndex));
    return name;
}

bool SaveRgbaToPng(const char* path, const unsigned char* rgba, u32 width, u32 height) {
    (void)path; (void)rgba; (void)width; (void)height;
    HE_CORE_WARN("[截图] 当前平台未实现 PNG 写出（仅 Windows/WIC 可用）");
    return false;
}

#endif // _WIN32

} // namespace he::rhi
