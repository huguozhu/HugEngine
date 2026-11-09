// ============================================================
// 01.Triangle.Subset —— Windows 验证入口
//
// 跑的是被鸿蒙复用的同一份 App（App/01.Triangle.App.cpp），用于在桌面
// 快速验证"三角形渲染子集"没有回归（鸿蒙侧无验证层，先在桌面看画面）。
// 不支持后端热切换（切换逻辑只在 01.Triangle 完整入口里）。
// ============================================================

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/IWindow.h"
#include "Platform/Time.h"
#include "Platform/IInput.h"
#include "RHI/RHI.h"

#include "01.Triangle.App.h"

using namespace he;

int main() {
    EngineConfig config;
    config.appName      = "HugEngine — 01.Triangle.Subset";
    config.windowWidth  = 960;
    config.windowHeight = 540;

    Engine engine(config);
    engine.Initialize();

    samples::triangle01::App app;
    if (!app.Initialize(engine, rhi::ResolveBackendFromEnv())) {
        HE_CORE_ERROR("01.Triangle.Subset: 初始化失败");
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
