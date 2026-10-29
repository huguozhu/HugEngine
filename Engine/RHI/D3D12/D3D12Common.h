// ============================================================================
//  D3D12Common.h —— D3D12 后端的公共头
//
//  职责：统一的平台头包含、COM 智能指针、错误检查宏、调试日志。
//  所有 D3D12 后端源文件都应先包含本文件。
// ============================================================================
#pragma once

#if defined(_WIN32)

// 引擎自己的 RHI 接口（先包含，避免与 d3d12.h 的宏冲突）
#include "RHI/RHI.h"
#include "RHI/Types.h"
#include "Core/Log.h"

// Windows / D3D12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>

// 需要 Agility SDK 的新特性时取消注释（本工程当前用系统自带 runtime）
#include <d3d12shader.h>
#include <cstdlib>   // std::getenv（TraceEnabled）
#include <string>    // std::wstring（Utf8ToWide）

namespace he::rhi {

/// COM 智能指针的简写（Microsoft::WRL::ComPtr）
template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

/// 逐调用跟踪开关（HE_D3D12_TRACE）：只在首次调用时读一次环境变量。
/// getenv 每次都要加锁并扫描整个环境块，不能放在每次绘制 / 设置状态的热路径上。
inline bool TraceEnabled() {
    static const bool s_Enabled = (std::getenv("HE_D3D12_TRACE") != nullptr);
    return s_Enabled;
}

/// UTF-8 → UTF-16：D3D12 的对象名（SetName）与 PIX 事件字符串都是宽字符，引擎的名字是 UTF-8（含中文）
inline std::wstring Utf8ToWide(const char* s) {
    if (!s || !*s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

/// D3D12 的 HRESULT 检查：失败时打印可读错误并返回 false
/// 用法：HE_D3D12_CHECK(device->CreateBuffer(&bd, nullptr, &buf), "CreateBuffer");
inline bool CheckHr(HRESULT hr, const char* what, const char* file, int line) {
    if (SUCCEEDED(hr)) return true;
    HE_CORE_ERROR("D3D12 [{}:{}] {} 失败 hr=0x{:08x}", file, line, what, static_cast<u32>(hr));
    return false;
}

/// 把 DXGI 的 HRESULT 转成可读文本（调试用）
inline const char* HrName(HRESULT hr) {
    switch (hr) {
        case E_INVALIDARG:                    return "E_INVALIDARG";
        case E_OUTOFMEMORY:                   return "E_OUTOFMEMORY";
        case E_FAIL:                          return "E_FAIL";
        case DXGI_ERROR_DEVICE_REMOVED:       return "DXGI_ERROR_DEVICE_REMOVED";
        case DXGI_ERROR_DEVICE_HUNG:          return "DXGI_ERROR_DEVICE_HUNG";
        case DXGI_ERROR_DEVICE_RESET:         return "DXGI_ERROR_DEVICE_RESET";
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR:return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
        case DXGI_ERROR_INVALID_CALL:         return "DXGI_ERROR_INVALID_CALL";
        case DXGI_ERROR_UNSUPPORTED:          return "DXGI_ERROR_UNSUPPORTED";
        default:                              return "UNKNOWN";
    }
}

} // namespace he::rhi

/// HRESULT 检查宏：失败时打印错误并提前返回 false
#define HE_D3D12_CHECK(expr, what)     ::he::rhi::CheckHr((expr), (what), __FILE__, __LINE__)

/// HRESULT 检查宏（返回值版本）：失败时打印错误并 return
#define HE_D3D12_CHECK_RET(expr, what)                            do {                                                              HRESULT _hr = (expr);                                         if (!::he::rhi::CheckHr(_hr, (what), __FILE__, __LINE__))             return;                                               } while (0)

/// HRESULT 检查宏（返回 nullptr 版本）
#define HE_D3D12_CHECK_RET_NULL(expr, what)                       do {                                                              HRESULT _hr = (expr);                                         if (!::he::rhi::CheckHr(_hr, (what), __FILE__, __LINE__))             return nullptr;                                       } while (0)

#endif // _WIN32
