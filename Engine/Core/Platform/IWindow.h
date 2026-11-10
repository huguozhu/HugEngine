#pragma once

// ============================================================
// IWindow.h —— 平台窗口抽象
//
// 【为什么需要】Window 之前直接用 GLFW：GLFW 在 Windows 上可用，但鸿蒙没有 GLFW，
// 窗口来自 XComponent 的 OHNativeWindow。把"窗口"抽成接口后：
//   Engine / 样例 / ImGui / CameraInput 只依赖 IWindow；
//   Windows 实现 = WindowWin32（GLFW），鸿蒙实现 = WindowOHOS（OHNativeWindow）。
// 平台差异（GLFW / OH_NativeXComponent）只出现在各自的实现文件里。
// ============================================================

#include "Core/Types.h"
#include "Math/Math.h"

#include <functional>
#include <memory>

namespace he {

/// 窗口创建参数（平台无关）
struct WindowDesc {
    String  title     = "HugEngine";
    u32     width     = kDefaultWindowWidth;
    u32     height    = kDefaultWindowHeight;
    bool    resizable = true;
    bool    vsync     = true;   // 只作为交换链的提示（由 RHI 使用）
};

class IWindow {
public:
    virtual ~IWindow() = default;

    using ResizeCallback = std::function<void(u32 width, u32 height)>;

    virtual bool ShouldClose() const = 0;
    /// 请求关闭窗口（等价于用户点标题栏 ×）：桌面端置关闭标志，移动端交给平台生命周期
    virtual void RequestClose() = 0;
    virtual void PollEvents() = 0;

    virtual u32    GetWidth()  const = 0;
    virtual u32    GetHeight() const = 0;
    virtual float2 GetSize()   const = 0;
    virtual float  GetAspectRatio() const = 0;

    /// 原生窗口句柄：Windows = HWND；鸿蒙 = OHNativeWindow*
    /// （仅用于 RHI 建设备 / 交换链表面，不参与窗口以外的逻辑）
    virtual void* GetNativeWindow() const = 0;

    /// 原生显示/实例句柄：Windows = HINSTANCE；鸿蒙 = nullptr
    virtual void* GetDisplayHandle() const = 0;

    /// 内容缩放（UI/字体密度）：Windows 恒为 1，移动端返回屏幕 density
    virtual f32 GetContentScale() const { return 1.0f; }

    /// 重建原生窗口（同尺寸/同位置）。
    /// 用途见历史说明：Windows 上先创建过 DXGI 交换链的 HWND 再让 Vulkan 呈现会冻结，
    /// 切到 Vulkan 前换一个干净的 HWND 即可。
    virtual bool Recreate() = 0;

    virtual void SetResizeCallback(ResizeCallback cb) = 0;

    /// 设置窗口标题（桌面端用于显示 FPS 等信息；移动端可为空实现）
    virtual void SetTitle(const char* title) = 0;

    /// 桌面端：隐藏/显示鼠标（相对鼠标模式）；移动端为空实现
    virtual void SetCursorVisible(bool visible) = 0;
};

/// 设置内容缩放（UI 密度）。移动端由平台条目在启动时调用（鸿蒙 display.densityPixels /
/// Android displayMetrics.density）；桌面端无实现（不调用）。
void SetContentScale(f32 scale);

/// 平台工厂：Windows 实现在 WindowWin32.cpp，鸿蒙实现在 WindowOHOS.cpp
std::unique_ptr<IWindow> CreatePlatformWindow(const WindowDesc& desc);

} // namespace he
