// ============================================================================
//  D3D11Common.h —— D3D11 后端的公共头
//
//  职责：统一的平台头包含、COM 智能指针、HRESULT 检查、调试开关。
//  所有 D3D11 后端源文件都应先包含本文件。
//
//  最低运行环境（见 docs/已实现功能/D3D11后端实现规划.md 1.3 节）：
//    Windows 10 1703+ 的 D3D11.4 运行时（ID3D11Device5 / ID3D11DeviceContext4）、
//    特性级别 11_1、常量缓冲偏移绑定、动态常量缓冲 NO_OVERWRITE、类型化 UAV 读取。
// ============================================================================
#pragma once

#if defined(_WIN32)

// 引擎自己的 RHI 接口（先包含，避免与 d3d11.h 的宏冲突）
#include "RHI/RHI.h"
#include "RHI/Types.h"
#include "Core/Log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wrl/client.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>

#include <cstdlib>
#include <string>

namespace he::rhi::d3d11 {

/// COM 智能指针的简写（Microsoft::WRL::ComPtr）
template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

/// 引擎的位标志枚举没有 HasFlag 辅助函数，这里自己写一个
template <typename E>
inline bool HasAny(E value, E bit) {
    return (static_cast<u32>(value) & static_cast<u32>(bit)) != 0u;
}

/// 逐调用跟踪开关（HE_D3D11_TRACE）：只在首次调用时读一次环境变量（不能放在热路径上反复 getenv）
inline bool TraceEnabled() {
    static const bool s_Enabled = (std::getenv("HE_D3D11_TRACE") != nullptr);
    return s_Enabled;
}

/// UTF-8 → UTF-16：调试名（SetPrivateData / 用户标注）是宽字符，引擎的名字是 UTF-8（含中文）
inline std::wstring Utf8ToWide(const char* s) {
    if (!s || !*s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

/// HRESULT 检查：失败时打印可读错误并返回 false
inline bool CheckHr(HRESULT hr, const char* what, const char* file, int line) {
    if (SUCCEEDED(hr)) return true;
    HE_CORE_ERROR("D3D11 [{}:{}] {} 失败 hr=0x{:08x}", file, line, what, static_cast<u32>(hr));
    return false;
}

} // namespace he::rhi::d3d11

/// HRESULT 检查宏：失败时打印错误，表达式结果为 bool
#define HE_D3D11_CHECK(expr, what) ::he::rhi::d3d11::CheckHr((expr), (what), __FILE__, __LINE__)

#endif // _WIN32
