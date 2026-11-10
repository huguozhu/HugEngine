// ============================================================
// 01.Triangle —— Windows 入口
//
// 只做配置 + 交给桌面宿主；主循环、输入、后端热切换都在
// Platforms/Win32/DesktopHost.cpp，场景与渲染在 01.Triangle.App.cpp。
// 鸿蒙入口（Platforms/OHOS/entry/src/main/cpp/AppMain.cpp）编同一份 App。
// ============================================================

#include "DesktopHost.h"

#include "01.Triangle.App.h"

int main() {
    he::samples::desktop::HostConfig cfg;
    cfg.title  = "HugEngine — 01.Triangle";
    cfg.width  = 960;
    cfg.height = 540;

    // 01 是唯一支持面板/环境变量热切换后端的样例
    cfg.backendSwitch = true;
    cfg.switchToEnv   = "HE_TRIANGLE_SWITCH_TO";
    cfg.switchBackEnv = "HE_TRIANGLE_SWITCH_BACK";

    return he::samples::desktop::RunApp<he::samples::triangle01::App>(cfg);
}
