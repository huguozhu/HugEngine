// ============================================================
// 03.Sponza.Subset —— Windows 验证入口
//
// 跑的是鸿蒙复用的同一份 App（App/03.Sponza.App.cpp）：
//   Sponza 场景 + 前向管线 + 天空盒 + 触摸相机（桌面用鼠标右键拖拽）
// 完整功能（RT 路径、配置读写、整套面板）仍在 03.Sponza-Forward。
// ============================================================

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/IWindow.h"
#include "Platform/Time.h"
#include "Platform/IInput.h"
#include "RHI/RHI.h"

#include "03.Sponza.App.h"

using namespace he;

int main() {
    EngineConfig config;
    config.appName      = "HugEngine — 03.Sponza.Subset";
    config.windowWidth  = 1280;
    config.windowHeight = 720;

    Engine engine(config);
    engine.Initialize();

    samples::sponza03::App app;
    if (!app.Initialize(engine, rhi::ResolveBackendFromEnv())) {
        HE_CORE_ERROR("03.Sponza.Subset: 初始化失败");
        return 1;
    }
    engine.GetWindow()->SetResizeCallback([&](u32 w, u32 h) { app.Resize(w, h); });

    f64 lastTime = he::Time::Now();
    while (!engine.GetWindow()->ShouldClose()) {
        engine.GetWindow()->PollEvents();
        const InputState& in = he::PollInput(engine.GetWindow());
        const f64 now = he::Time::Now();
        const f32 dt  = static_cast<f32>(now - lastTime);
        lastTime      = now;
        app.Frame(dt, in);
    }

    app.Shutdown();
    return 0;
}
