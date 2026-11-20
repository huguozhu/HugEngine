// ============================================================
//  DesktopHost.cpp —— Windows 样例宿主
//
//  从原 Samples/01.Triangle/01.Triangle.cpp 的 main 抽出来：建 Engine/窗口、
//  主循环、输入、后端热切换、退出。之后所有桌面样例都只写一个 App，
//  宿主按能力选择两种循环：
//    · 单线程：IApp::Frame（默认，01/02/04 这类）
//    · 渲染线程：IRenderThreadApp 的 BeginFrame / RecordScene / RecordUIAndPresent
//      （整帧 RHI 交给 RenderCommandQueue + RenderThread，游戏线程只做输入/UI）
// ============================================================

#include "DesktopHost.h"

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/IWindow.h"
#include "Platform/Time.h"
#include "Platform/IInput.h"
#include "RHI/ThreadAffinity.h"
#include "Threading/RenderThread.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace he::samples::desktop {
namespace {

const char* BackendName(rhi::Backend b) {
    switch (b) {
        case rhi::Backend::Vulkan: return "Vulkan";
        case rhi::Backend::D3D12:  return "D3D12";
        case rhi::Backend::D3D11:  return "D3D11";
        default:                   return "Unknown";
    }
}

/// 环境变量里的后端名 → 枚举（大小写不敏感，与 rhi::ResolveBackendFromEnv 的写法保持一致）
rhi::Backend BackendFromName(const char* name) {
    if (!name || !*name) return rhi::Backend::Count;
    char lower[16]{};
    std::size_t i = 0;
    for (; name[i] && i + 1 < sizeof(lower); ++i) {
        lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
    }
    if (std::strcmp(lower, "vulkan") == 0) return rhi::Backend::Vulkan;
    if (std::strcmp(lower, "d3d12") == 0 || std::strcmp(lower, "dx12") == 0) return rhi::Backend::D3D12;
    if (std::strcmp(lower, "d3d11") == 0 || std::strcmp(lower, "dx11") == 0) return rhi::Backend::D3D11;
    return rhi::Backend::Count;
}

// ---------------------------------------------------------------------------
//  单线程循环：App 自己在 Frame 里 acquire/record/present
// ---------------------------------------------------------------------------
u64 RunSingleThreaded(Engine& engine, IApp& app, IBackendSwitchApp* switchable,
                      const HostConfig& cfg) {
    f64 lastTime   = he::Time::Now();
    u64 frameIndex = 0;

    while (!engine.GetWindow()->ShouldClose()) {
        engine.GetWindow()->PollEvents();
        const InputState& in = he::PollInput(engine.GetWindow());

        const f64 now = he::Time::Now();
        const f32 dt  = static_cast<f32>(now - lastTime);
        lastTime      = now;

        if (!app.Frame(dt, in)) continue;
        ++frameIndex;

        // 【T8 取证】`HE_CAPTURE_FRAME=N` → 第 N 帧请求**引擎侧**截图（swapchain 抓取）。
        // 【为什么不用窗口抓屏】实测 `CopyFromScreen` 在 Vulkan 交换链窗口上只能抓到纯白，
        // 会得出"渲染失败"的错误结论；引擎自己的 `RequestScreenshot` 才是可信口径。
        if (const char* capEnv = std::getenv("HE_CAPTURE_FRAME")) {
            static u32 s_CapFrame = 0u;
            const int target = std::atoi(capEnv);
            if (++s_CapFrame == static_cast<u32>(target > 0 ? target : 0)) {
                const char* path = std::getenv("HE_CAPTURE_PATH");
                app.RequestScreenshot(path != nullptr ? path : "capture_host.png");
            }
        }

        if (!cfg.backendSwitch || !switchable) continue;

        // ---- 无人值守验证热切换：第 60 帧切走、第 90 帧切回（环境变量给目标后端名）----
        rhi::Backend want = rhi::Backend::Count;
        if (cfg.switchToEnv && frameIndex == 60) {
            if (const char* n = std::getenv(cfg.switchToEnv)) want = BackendFromName(n);
        }
        if (cfg.switchBackEnv && frameIndex == 90) {
            if (const char* n = std::getenv(cfg.switchBackEnv)) want = BackendFromName(n);
        }
        // 面板里选的后端
        rhi::Backend panelWanted = rhi::Backend::Count;
        if (switchable->TakeBackendRequest(panelWanted)) want = panelWanted;

        if (want != rhi::Backend::Count && want != switchable->CurrentBackend()) {
            HE_CORE_INFO("{}: 切换到 {}", cfg.title, BackendName(want));
            app.Shutdown();
            // 窗口上先有 DXGI 翻转交换链再让 Vulkan 呈现，桌面会停在 D3D 最后一帧
            //（Present 成功但画面不更新）⇒ 切到 Vulkan 前换一个干净的 HWND。
            if (want == rhi::Backend::Vulkan) engine.GetWindow()->Recreate();
            if (!app.Initialize(engine, want, IApp::Options{})) {
                HE_CORE_ERROR("{}: 切换到 {} 失败，退出", cfg.title, BackendName(want));
                break;
            }
        }
    }
    return frameIndex;
}

// ---------------------------------------------------------------------------
//  渲染线程循环：一帧两条命令
//    ① Acquire + 场景录制（含 UI 的 render pass 开始）
//    ② UI 录制 + End + Submit + Present
//  控件/相机等 CPU 侧逻辑留在游戏线程、位于两条命令之间（与完整版样例一致）。
// ---------------------------------------------------------------------------
u64 RunWithRenderThread(Engine& engine, IRenderThreadApp& app, const HostConfig& cfg) {
    render::RenderCommandQueue queue(rhi::kMaxFramesInFlight);
    render::RenderThread       renderThread(queue);
    render::FrameScheduler     frameScheduler(queue, renderThread);

    renderThread.SetSpinWaitUs(50);
    renderThread.SetThreadStartHook([] { he::rhi::GetThreadAffinity().Claim(); });
    renderThread.SetThreadStopHook([] { he::rhi::GetThreadAffinity().Release(); });
    renderThread.Start();
    HE_CORE_INFO("{}: 已启动渲染线程（整帧 RHI 归它，游戏线程按帧握手）", cfg.title);

    auto submit = [&](auto&& fn) {
        queue.BeginFrame();
        queue.Enqueue([&](render::RenderThreadContext&) { fn(); });
        bool timedOut = false;
        frameScheduler.SubmitAndWait(timedOut);
        if (timedOut) HE_CORE_WARN("{}: 等待本帧渲染命令超时", cfg.title);
    };

    f64 lastTime   = he::Time::Now();
    u64 frameIndex = 0;

    while (!engine.GetWindow()->ShouldClose()) {
        engine.GetWindow()->PollEvents();
        const InputState& in = he::PollInput(engine.GetWindow());

        const f64 now = he::Time::Now();
        const f32 dt  = static_cast<f32>(now - lastTime);
        lastTime      = now;

        // 游戏线程：输入/相机/UI 状态；返回 false 表示本帧不渲染
        if (!app.BeginFrame(dt, in)) continue;

        bool acquireFailed = false;
        submit([&] { acquireFailed = !app.RecordScene(); });
        if (acquireFailed) continue;

        // 【T8 取证】多线程路径的截图由应用自己在 `RecordUIAndPresent` 里做
        // （`IRenderThreadApp` 没有 `RequestScreenshot`；游戏线程的 `IApp` 才有）。
        submit([&] { app.RecordUIAndPresent(); });
        ++frameIndex;
    }

    renderThread.Stop();
    rhi::GetThreadAffinity().Release();
    return frameIndex;
}

} // namespace

int Run(const HostConfig& cfg, IApp& app, IBackendSwitchApp* switchable) {
    EngineConfig engineCfg;
    engineCfg.appName      = cfg.title;
    engineCfg.windowWidth  = cfg.width;
    engineCfg.windowHeight = cfg.height;

    Engine engine(engineCfg);
    engine.Initialize();

    if (!app.Initialize(engine, rhi::ResolveBackendFromEnv(), IApp::Options{})) {
        HE_CORE_ERROR("{}: 初始后端初始化失败", cfg.title);
        return 1;
    }

    // 窗口缩放：交换链与离屏 RT 目标都要跟着换
    engine.GetWindow()->SetResizeCallback([&](u32 w, u32 h) { app.Resize(w, h); });

    HE_CORE_INFO("{} started（{}）", cfg.title,
                 BackendName(switchable ? switchable->CurrentBackend() : rhi::Backend::Vulkan));

    // 渲染线程需要 App 实现 IRenderThreadApp；没有实现就退回单线程（行为与以前一致）
    const bool wantThread = cfg.renderThread || std::getenv("HE_RENDER_THREAD_STRICT") != nullptr;
    auto* rtApp = dynamic_cast<IRenderThreadApp*>(&app);

    u64 frames = 0;
    if (wantThread && rtApp) {
        frames = RunWithRenderThread(engine, *rtApp, cfg);
    } else {
        if (wantThread && !rtApp) {
            HE_CORE_WARN("{}: 请求了渲染线程，但该 App 没实现 IRenderThreadApp，退回单线程", cfg.title);
        }
        frames = RunSingleThreaded(engine, app, switchable, cfg);
    }

    app.Shutdown();
    HE_CORE_INFO("Exiting after {} frames", frames);
    return 0;
}

} // namespace he::samples::desktop
