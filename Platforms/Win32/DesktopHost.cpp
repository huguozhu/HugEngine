// ============================================================
//  DesktopHost.cpp —— Windows 样例宿主
//
//  从原 Samples/01.Triangle/01.Triangle.cpp 的 main 抽出来：建 Engine/窗口、
//  主循环、输入、后端热切换、退出。之后所有桌面样例都只写 Main.Win32.cpp
//  （配置 + RunApp<TApp>），不再各写一份循环。
// ============================================================

#include "DesktopHost.h"

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/IWindow.h"
#include "Platform/Time.h"
#include "Platform/IInput.h"

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

    app.Shutdown();
    HE_CORE_INFO("Exiting after {} frames", frameIndex);
    return 0;
}

} // namespace he::samples::desktop
