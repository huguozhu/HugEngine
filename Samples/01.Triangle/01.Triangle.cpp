// ============================================================
// 01.Triangle — 桌面入口（渲染代码在 App/01.Triangle.App.cpp，与鸿蒙共用）
//
// 这里只负责：
//   · 引擎/窗口生命周期
//   · 主循环（输入快照 → App::Frame）
//   · 后端热切换（RHI 只允许一个设备 ⇒ 整个 App 重建）
//   · 自动化开关：HE_TRIANGLE_MODE / HE_TRIANGLE_SWITCH_TO / HE_TRIANGLE_SWITCH_BACK
// ============================================================

#include "Core/Core.h"
#include "Core/Engine.h"
#include "Platform/IWindow.h"
#include "Platform/Time.h"
#include "Platform/IInput.h"
#include "RHI/RHI.h"
#include "Editor/ImGuiIntegration.h"

#include "01.Triangle.App.h"

#include <cstdlib>
#include <cstring>

using namespace he;

namespace {

const char* BackendName(rhi::Backend b) {
    switch (b) {
        case rhi::Backend::Vulkan: return "Vulkan";
        case rhi::Backend::D3D12:  return "D3D12";
        case rhi::Backend::D3D11:  return "D3D11";
        default:                   return "Unknown";
    }
}

} // namespace

int main() {
    EngineConfig config;
    config.appName      = "HugEngine — 01.Triangle";
    config.windowWidth  = 960;
    config.windowHeight = 540;

    Engine engine(config);
    engine.Initialize();

    samples::triangle01::App app;
    if (!app.Initialize(engine, rhi::ResolveBackendFromEnv())) {
        HE_CORE_ERROR("01.Triangle: 初始后端初始化失败");
        return 1;
    }

    // 窗口缩放：交换链与离屏 RT 目标都要跟着换
    engine.GetWindow()->SetResizeCallback([&](u32 w, u32 h) { app.Resize(w, h); });

    HE_CORE_INFO("01.Triangle started — ImGui 切换渲染模式 / 渲染后端（当前 {}）",
                 BackendName(app.CurrentBackend()));

    f64 lastTime = he::Time::Now();
    u64 frameIndex = 0;

    while (!engine.GetWindow()->ShouldClose()) {
        engine.GetWindow()->PollEvents();
        const InputState& in = he::PollInput(engine.GetWindow());

        const f64 now  = he::Time::Now();
        const f32 dt   = static_cast<f32>(now - lastTime);
        lastTime       = now;

        if (!app.Frame(dt, in)) continue;
        ++frameIndex;

        // ============================================================
        // 自动化开关（无人值守验证热切换：第 60 帧切走、第 90 帧切回）
        // ============================================================
        auto backendFromName = [](const char* n) {
            if (std::strcmp(n, "vulkan") == 0) return rhi::Backend::Vulkan;
            if (std::strcmp(n, "d3d12")  == 0) return rhi::Backend::D3D12;
            if (std::strcmp(n, "d3d11")  == 0) return rhi::Backend::D3D11;
            return rhi::Backend::Count;
        };
        rhi::Backend want = rhi::Backend::Count;
        if (const char* sw = std::getenv("HE_TRIANGLE_SWITCH_TO")) {
            if (frameIndex == 60) want = backendFromName(sw);
        }
        if (const char* swb = std::getenv("HE_TRIANGLE_SWITCH_BACK")) {
            if (frameIndex == 90) want = backendFromName(swb);
        }
        // 面板里选的后端
        rhi::Backend panelWanted = rhi::Backend::Count;
        if (app.TakeBackendRequest(panelWanted)) want = panelWanted;

        if (want != rhi::Backend::Count && want != app.CurrentBackend()) {
            HE_CORE_INFO("01.Triangle: 切换到 {}", BackendName(want));
            app.Shutdown();
            // 窗口上先有 DXGI 翻转交换链再让 Vulkan 呈现，桌面会停在 D3D 最后一帧
            // （Present 成功但画面不更新）⇒ 切到 Vulkan 前换一个干净的 HWND。
            if (want == rhi::Backend::Vulkan) engine.GetWindow()->Recreate();
            if (!app.Initialize(engine, want)) {
                HE_CORE_ERROR("01.Triangle: 切换到 {} 失败，退出", BackendName(want));
                break;
            }
        }
    }

    app.Shutdown();
    HE_CORE_INFO("Exiting after {} frames", frameIndex);
    return 0;
}
