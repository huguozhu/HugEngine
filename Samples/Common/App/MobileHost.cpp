// ============================================================
// MobileHost.cpp —— 移动端共享宿主实现（鸿蒙 / Android 共用）
//
// 【职责边界】
//   · 起/停渲染线程、维护"待处理尺寸"、把触摸事件交给引擎输入队列；
//   · 按 mode 选择样例 App 并驱动 IApp 的每帧调用；
//   · 平台无关：窗口挂接（AttachNativeWindow）与事件桥接（napi / JNI）
//     都由各平台入口完成，本文件不出现任何平台 API。
//
// 【与桌面宿主的关系】桌面用 Platforms/Win32/DesktopHost（HostConfig + Run），
// 移动端没有可自由创建的窗口，所以单独一条"线程 + surface 挂接"的路径。
// ============================================================

#include "App/MobileHost.h"

#include "Core/Platform.h"
#include "Core/Engine.h"
#include "Core/Log.h"
#include "Platform/IInput.h"
#include "Platform/IWindow.h"
#include "Platform/Time.h"
#include "RHI/RHI.h"

#include "01.Triangle.App.h"
#include "02.Cube.App.h"

// Android 首版只打包 01/02（Sponza 资产约 56MB，暂不随包）；
// 由 entry 的 CMake 定义 HE_MOBILE_MAX_MODE=2 来裁掉 03/04。
#ifndef HE_MOBILE_MAX_MODE
    #define HE_MOBILE_MAX_MODE 4
#endif

#if HE_MOBILE_MAX_MODE >= 3
    #include "03.Sponza.App.h"
#endif
#if HE_MOBILE_MAX_MODE >= 4
    #include "04.Deferred.App.h"
#endif

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace he::samples::mobile {
namespace {

std::atomic<bool> g_Running{ false };
std::thread       g_Thread;
std::atomic<u32>  g_PendingW{ 0 };
std::atomic<u32>  g_PendingH{ 0 };
std::atomic<u32>  g_SurfaceW{ 1 };
std::atomic<u32>  g_SurfaceH{ 1 };
std::atomic<int>  g_Mode{ 1 };

// 当前 App 的截图入口：不同样例的 App 类型不同，用类型擦除的回调转发
std::function<void(const char*)> g_Screenshot;

/// 通用循环：所有 App 的接口一致（Initialize/Frame/Resize/Shutdown）
template <typename TApp>
void RunAppLoop(Engine& engine, IWindow* window, const char* appName, const char* hostName) {
    TApp app;
    typename TApp::Options opt;
    opt.enableImGui = true;   // 触摸输入后端见 Engine/Editor/Editor/imgui_impl_ohos.cpp

    if (!app.Initialize(engine, rhi::Backend::Vulkan, opt)) {
        HE_CORE_ERROR("[{}] {} 初始化失败", hostName, appName);
        return;
    }

    g_SurfaceW.store(window->GetWidth());
    g_SurfaceH.store(window->GetHeight());

    // 按需截图：把当前 App 的截帧入口挂到全局回调（App 析构前清掉）
    g_Screenshot = [&app](const char* path) { app.RequestScreenshot(path); };

    f64 lastTime = Time::Now();
    HE_CORE_INFO("[{}] 渲染循环启动: {}（{}x{}）", hostName, appName,
                 window->GetWidth(), window->GetHeight());

    while (g_Running.load() && !window->ShouldClose()) {
        window->PollEvents();

        // 平台线程投递的尺寸变化（旋转 / 分屏）
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

    HE_CORE_INFO("[{}] 渲染循环退出: {}", hostName, appName);
    g_Screenshot = nullptr;
    app.Shutdown();
}

void RenderLoop(u32 width, u32 height, int mode, std::string hostName) {
    const int maxMode = HE_MOBILE_MAX_MODE;

    // 样例名与窗口标题
    const char* appName = "01.Triangle";
    if (mode == 2)      appName = "02.Cube";
#if HE_MOBILE_MAX_MODE >= 3
    else if (mode == 3) appName = "03.Sponza";
#endif
#if HE_MOBILE_MAX_MODE >= 4
    else if (mode == 4) appName = "04.Deferred";
#endif
    else if (mode < 1 || mode > maxMode) {
        appName = "01.Triangle";
    }

    const std::string title = std::string("HugEngine — ") + appName + " (" + hostName + ")";

    EngineConfig cfg;
    cfg.appName      = title.c_str();
    cfg.windowWidth  = width;
    cfg.windowHeight = height;
    cfg.enableVSync  = true;
    cfg.logLevel     = LogLevel::Info;

    Engine engine(cfg);
    engine.Initialize();

    IWindow* window = engine.GetWindow();
    if (!window || window->GetNativeWindow() == nullptr) {
        HE_CORE_ERROR("[{}] 没有可用的原生窗口，渲染线程退出", hostName);
        return;
    }

    const char* host = hostName.c_str();
    if (mode == 1) {
        RunAppLoop<triangle01::App>(engine, window, "01.Triangle", host);
    } else if (mode == 2) {
        RunAppLoop<cube02::App>(engine, window, "02.Cube", host);
    }
#if HE_MOBILE_MAX_MODE >= 3
    else if (mode == 3) {
        RunAppLoop<sponza03::App>(engine, window, "03.Sponza", host);
    }
#endif
#if HE_MOBILE_MAX_MODE >= 4
    else if (mode == 4) {
        RunAppLoop<sponza04::App>(engine, window, "04.Deferred", host);
    }
#endif
}

} // namespace

void Start(u32 width, u32 height, int mode, const char* hostName) {
    if (g_Running.load()) return;

    const int maxMode = HE_MOBILE_MAX_MODE;
    g_Mode.store((mode >= 1 && mode <= maxMode) ? mode : 1);

    g_Running.store(true);
    g_PendingW.store(0);
    g_PendingH.store(0);
    g_SurfaceW.store(width  ? width  : 1);
    g_SurfaceH.store(height ? height : 1);
    g_Thread = std::thread(RenderLoop,
                           width ? width : 960,
                           height ? height : 540,
                           g_Mode.load(),
                           std::string(hostName ? hostName : "Mobile"));
}

bool IsRunning() {
    return g_Running.load();
}

int CurrentMode() {
    return g_Mode.load();
}

void Stop() {
    g_Running.store(false);
    if (g_Thread.joinable()) g_Thread.join();
}

void Resize(u32 width, u32 height) {
    if (width == 0 || height == 0) return;
    g_PendingW.store(width);
    g_PendingH.store(height);
}

// 【坐标契约】入口传进来的是**归一化**坐标 [0,1]。这里不要再除以窗口尺寸 ——
// 鸿蒙早期多除了一次，导致真机上所有触摸都被压成 (0,0)，相机完全没反应。
void Touch(u32 id, int phase, float xNorm, float yNorm) {
    const auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    PushTouchPoint(id, static_cast<TouchPhase>(phase), clamp01(xNorm), clamp01(yNorm));
}

bool Screenshot(const char* path) {
    if (!g_Screenshot || !path || !*path) return false;
    g_Screenshot(path);
    return true;
}

} // namespace he::samples::mobile
