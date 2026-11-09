// ============================================================
// 04.Deferred.Subset —— Windows 验证入口
//
// 跑的是鸿蒙复用的同一份 App（App/04.Deferred.App.cpp）：
//   Sponza + 延迟管线（GBuffer → 全屏 Lighting → ToneMap）+ 触摸相机
// 完整功能（SSGI/DDGI/SSR/SSAO/粒子/配置）仍在 04.Sponza-Deferred。
// ============================================================

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/IWindow.h"
#include "Platform/Time.h"
#include "Platform/IInput.h"
#include "RHI/RHI.h"

#include "04.Deferred.App.h"

using namespace he;

int main() {
    EngineConfig config;
    config.appName      = "HugEngine — 04.Deferred.Subset";
    config.windowWidth  = 1280;
    config.windowHeight = 720;

    Engine engine(config);
    engine.Initialize();

    samples::sponza04::App app;
    if (!app.Initialize(engine, rhi::ResolveBackendFromEnv())) {
        HE_CORE_ERROR("04.Deferred.Subset: 初始化失败");
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
