#pragma once

// ============================================================
//  IApp —— 所有样例实现的统一接口（场景 + 渲染 + 面板）
//
//  样例只写「场景与渲染」，平台相关的生命周期（建窗口、主循环、输入轮询、
//  XComponent/napi）放在各平台宿主里：
//    · 桌面：Platforms/Win32/DesktopHost.{h,cpp}
//    · 鸿蒙：Platforms/OHOS/entry/src/main/cpp/{NativeEntry,AppMain}.cpp
//  宿主只依赖这个接口，不依赖具体样例 ⇒ 新增样例不用改宿主。
// ============================================================

#include "Core/Core.h"
#include "RHI/RHI.h"

namespace he {

class Engine;
struct InputState;

namespace samples {

class IApp {
public:
    /// 初始化选项（保持聚合体：调用方可用 `{ .enableImGui = false }`）
    struct Options {
        bool enableImGui = true;   ///< 是否创建 ImGui
    };

    virtual ~IApp() = default;

    /// 建设备 / 交换链 / 场景 / 管线（可重复调用：实现里先 Shutdown）
    virtual bool Initialize(Engine& engine, rhi::Backend backend, const Options& opt) = 0;

    /// 每帧：相机输入 + 渲染 + 面板 + Present；返回 false 表示本帧无画面（Acquire 失败等）
    virtual bool Frame(f32 deltaTime, const InputState& input) = 0;

    virtual void Resize(u32 width, u32 height) = 0;
    virtual void RequestScreenshot(const char* path) = 0;
    virtual void Shutdown() = 0;
    virtual bool IsReady() const = 0;
};

/// 可选能力：支持在运行时切换渲染后端（目前只有 01.Triangle）。
/// 宿主用 `if constexpr` 判断，不实现它的样例不受影响。
class IBackendSwitchApp {
public:
    virtual ~IBackendSwitchApp() = default;

    /// 当前后端
    virtual rhi::Backend CurrentBackend() const = 0;

    /// 面板里选中的目标后端；返回 false 表示本帧没有切换请求
    virtual bool TakeBackendRequest(rhi::Backend& out) = 0;
};

/// 可选能力：把一帧拆成「游戏线程准备 → 命令 1（场景录制）→ 命令 2（UI + Present）」，
/// 宿主据此把两条命令派发到渲染线程（`HostConfig::renderThread`）。不实现它的样例
/// 仍走单线程的 `IApp::Frame`。
///
/// 这样渲染线程只属于宿主一份实现，样例不必各自重复线程/队列代码。
class IRenderThreadApp {
public:
    virtual ~IRenderThreadApp() = default;

    /// 游戏线程：输入/相机/UI 状态更新；返回 false 表示本帧不渲染（Acquire 前就放弃）
    virtual bool BeginFrame(f32 deltaTime, const InputState& input) = 0;

    /// 命令 1（渲染线程）：Acquire + 场景录制 + 打开 UI 的 render pass；
    /// 返回 false 表示 Acquire 失败（宿主会跳过命令 2）
    virtual bool RecordScene() = 0;

    /// 命令 2（渲染线程）：录制 UI + End + Submit + Present
    virtual void RecordUIAndPresent() = 0;
};

} // namespace samples
} // namespace he
