// ============================================================
// AppMain.cpp —— 鸿蒙端渲染循环（01.Triangle / 02.Cube 可切换）
//
// 两个样例的渲染逻辑都在引擎侧的 App 里（与 Windows 共用同一份源码）：
//   · 01.Triangle → Samples/01.Triangle/App/01.Triangle.App.cpp
//   · 02.Cube     → Samples/02.Cube/App/02.Cube.App.cpp
//
// 本文件只负责鸿蒙特有的三件事：起渲染线程、转发 resize、转发触摸，
// 以及"切换样例"（AppStop → AppStart，WindowOHOS 的原生窗口保持不变）。
// ============================================================

#include "Core/Platform.h"

#include "AppMain.h"

#if HE_PLATFORM_OHOS

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/IInput.h"
#include "Platform/InputOHOS.h"   // PushTouchEvent（触摸注入）
#include "Platform/IWindow.h"
#include "Platform/Time.h"
#include "RHI/RHI.h"

#include "01.Triangle.App.h"
#include "02.Cube.App.h"

#include <atomic>
#include <thread>

namespace he::ohos {
namespace {

std::atomic<bool> g_Running{ false };
std::thread       g_Thread;
std::atomic<u32>  g_PendingW{ 0 };
std::atomic<u32>  g_PendingH{ 0 };
std::atomic<u32>  g_SurfaceW{ 1 };
std::atomic<u32>  g_SurfaceH{ 1 };
std::atomic<int>  g_Mode{ 1 };   // 1 = 01.Triangle（默认），2 = 02.Cube

/// 通用循环：两个 App 的接口一致（Initialize/Frame/Resize/Shutdown）
template <typename TApp>
void RunAppLoop(Engine& engine, IWindow* window, const char* appName) {
    TApp app;
    typename TApp::Options opt;
    opt.enableImGui = true;   // 触摸输入后端见 Engine/Editor/Editor/imgui_impl_ohos.cpp

    if (!app.Initialize(engine, rhi::Backend::Vulkan, opt)) {
        HE_CORE_ERROR("[OHOS] {} 初始化失败", appName);
        return;
    }

    g_SurfaceW.store(window->GetWidth());
    g_SurfaceH.store(window->GetHeight());

    f64 lastTime = Time::Now();
    HE_CORE_INFO("[OHOS] 渲染循环启动: {}（{}x{}）", appName, window->GetWidth(), window->GetHeight());

    while (g_Running.load() && !window->ShouldClose()) {
        window->PollEvents();

        // ArkTS 线程投递的尺寸变化（旋转 / 分屏）
        const u32 pw = g_PendingW.exchange(0);
        const u32 ph = g_PendingH.exchange(0);
        if (pw && ph) {
            app.Resize(pw, ph);
            g_SurfaceW.store(pw);
            g_SurfaceH.store(ph);
        }

        InputState& in = PollInput(window);

        const f64 now = Time::Now();
        const f32 dt  = static_cast<f32>(now - lastTime);
        lastTime = now;

        app.Frame(dt, in);
    }

    HE_CORE_INFO("[OHOS] 渲染循环退出: {}", appName);
    app.Shutdown();
}

void RenderLoop(u32 width, u32 height, int mode) {
    const bool triangle = (mode != 2);

    EngineConfig cfg;
    cfg.appName      = triangle ? "HugEngine — 01.Triangle (OHOS)" : "HugEngine — 02.Cube (OHOS)";
    cfg.windowWidth  = width;
    cfg.windowHeight = height;
    cfg.enableVSync  = true;
    cfg.logLevel     = LogLevel::Info;

    Engine engine(cfg);
    engine.Initialize();

    IWindow* window = engine.GetWindow();
    if (!window || window->GetNativeWindow() == nullptr) {
        HE_CORE_ERROR("[OHOS] 没有可用的原生窗口，渲染线程退出");
        return;
    }

    if (triangle) {
        RunAppLoop<samples::triangle01::App>(engine, window, "01.Triangle");
    } else {
        RunAppLoop<samples::cube02::App>(engine, window, "02.Cube");
    }
}

} // namespace

void AppStart(u32 width, u32 height, int mode) {
    if (g_Running.load()) return;
    g_Mode.store(mode == 2 ? 2 : 1);

    g_Running.store(true);
    g_PendingW.store(0);
    g_PendingH.store(0);
    g_SurfaceW.store(width  ? width  : 1);
    g_SurfaceH.store(height ? height : 1);
    g_Thread = std::thread(RenderLoop, width ? width : 960, height ? height : 540, g_Mode.load());
}

int AppCurrentMode() {
    return g_Mode.load();
}

bool AppIsRunning() {
    return g_Running.load();
}

void AppStop() {
    g_Running.store(false);
    if (g_Thread.joinable()) g_Thread.join();
}

void AppResize(u32 width, u32 height) {
    if (width == 0 || height == 0) return;
    g_PendingW.store(width);
    g_PendingH.store(height);
}

// 【坐标契约】ArkTS 侧传进来的是**归一化**坐标 [0,1]（TouchObject 与组件尺寸都以 vp 计，
// 比值与单位无关）。这里不要再除以窗口尺寸 —— 之前多除了一次，导致真机上所有触摸
// 都被压成 (0,0)，相机完全没反应。
void AppTouch(u32 id, int phase, float xNorm, float yNorm) {
    const auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    PushTouchEvent(id, static_cast<TouchPhase>(phase), clamp01(xNorm), clamp01(yNorm));
}

} // namespace he::ohos

#endif // HE_PLATFORM_OHOS
