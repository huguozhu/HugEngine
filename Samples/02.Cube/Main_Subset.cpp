// ============================================================
// Main_Subset.cpp —— Windows 侧入口：跑 02 的可移植子集（02.Cube.App）
//
// 用途：
//   1. 验证"鸿蒙也要用的那份 App 代码"在 Windows 上确实能出画面（改动前先在这里回归）
//   2. 鸿蒙入口（Platforms/OHOS/entry/src/main/cpp/AppMain.cpp）与本文件只差
//      "窗口/输入从哪来"，渲染逻辑完全一致
// ============================================================

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/IInput.h"
#include "Platform/IWindow.h"
#include "Platform/Time.h"
#include "RHI/RHI.h"

#include "02.Cube.App.h"

using namespace he;

int main() {
    EngineConfig cfg;
    cfg.appName      = "HugEngine — 02.Cube (Subset)";
    cfg.windowWidth  = 960;
    cfg.windowHeight = 540;
    cfg.enableVSync  = true;
    cfg.logLevel     = LogLevel::Info;

    Engine engine(cfg);
    engine.Initialize();

    samples::cube02::App app;
    if (!app.Initialize(engine, rhi::ResolveBackendFromEnv())) {
        HE_CORE_ERROR("02.Cube.Subset: 初始化失败");
        return 1;
    }

    engine.GetWindow()->SetResizeCallback([&](u32 w, u32 h) { app.Resize(w, h); });

    f64 lastTime = Time::Now();
    while (!engine.GetWindow()->ShouldClose()) {
        engine.GetWindow()->PollEvents();
        InputState& in = PollInput(engine.GetWindow());

        const f64 now = Time::Now();
        const f32 dt  = static_cast<f32>(now - lastTime);
        lastTime = now;

        app.Frame(dt, in);
    }

    app.Shutdown();
    HE_CORE_INFO("02.Cube.Subset 退出");
    return 0;
}
