#pragma once

// ============================================================
//  DesktopHost —— Windows 样例宿主（平台相关代码的唯一落点）
//
//  职责：建 Engine/窗口 → 初始化 App → 主循环（事件/输入/时间）→ 后端热切换
//        → 退出清理。样例侧只提供 `IApp`（场景与渲染）。
//
//  对应鸿蒙宿主：Platforms/OHOS/entry/src/main/cpp/AppMain.cpp（XComponent + napi）。
//  两边调用同一个 App 实现，差异只在宿主。
// ============================================================

#include "App/IApp.h"

#include <type_traits>

namespace he::samples::desktop {

struct HostConfig {
    const char* title  = "HugEngine";
    u32         width  = 1280;
    u32         height = 720;

    /// 是否启用后端热切换（App 需实现 IBackendSwitchApp，否则此开关无效）
    bool backendSwitch = false;

    /// 整帧渲染交给渲染线程（App 需实现 IRenderThreadApp，否则忽略此开关）。
    /// 环境变量 HE_RENDER_THREAD_STRICT 也会打开它，便于在命令行临时验证。
    bool renderThread = false;

    /// 无人值守验证热切换用的环境变量名（帧 60 切走、帧 90 切回）；
    /// 传 nullptr 表示不启用。例如 01.Triangle 用 "HE_TRIANGLE_SWITCH_TO"。
    const char* switchToEnv   = nullptr;
    const char* switchBackEnv = nullptr;
};

/// 宿主主循环（非模板，供 .cpp 实现；样例侧一般用下面的 RunApp）
int Run(const HostConfig& cfg, IApp& app, IBackendSwitchApp* switchable);

/// 样例侧入口：构造 App 并跑主循环
template <class TApp>
inline int RunApp(const HostConfig& cfg) {
    TApp app;
    if constexpr (std::is_base_of_v<IBackendSwitchApp, TApp>) {
        return Run(cfg, app, static_cast<IBackendSwitchApp*>(&app));
    } else {
        return Run(cfg, app, nullptr);
    }
}

} // namespace he::samples::desktop
