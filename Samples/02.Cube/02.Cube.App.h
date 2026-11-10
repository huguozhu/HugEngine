#pragma once

// ============================================================
// 02.Cube —— 场景与渲染（桌面与鸿蒙共用同一份）
//
// 立方体 + 球体 + 地面 + 天空盒 + Forward 管线 + 触摸相机子集。
//
// 平台相关的东西都在宿主里：
//   · 桌面：Platforms/Win32/DesktopHost（入口在本文件末尾的 HE_DESKTOP_ENTRY 段）
//   · 鸿蒙：Platforms/OHOS/entry/src/main/cpp/AppMain.cpp
// ============================================================

#include "App/IApp.h"

#include <memory>

namespace he {
class Engine;
struct InputState;
}

namespace he::samples::cube02 {

class App : public IApp, public IRenderThreadApp {
public:
    using Options = IApp::Options;

    App();
    ~App() override;

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    bool Initialize(Engine& engine, rhi::Backend backend) {
        return Initialize(engine, backend, Options{});
    }
    bool Initialize(Engine& engine, rhi::Backend backend, const Options& opt) override;

    /// 单线程路径：BeginFrame + RecordScene + RecordUIAndPresent（鸿蒙宿主用）
    bool Frame(f32 deltaTime, const InputState& input) override;

    // ---- IRenderThreadApp：桌面宿主把后两段派发到渲染线程 ----
    bool BeginFrame(f32 deltaTime, const InputState& input) override;
    bool RecordScene() override;
    void RecordUIAndPresent() override;

    void Resize(u32 width, u32 height) override;

    /// 请求截取下一帧到 path（PNG），用于"分享当前画面"
    void RequestScreenshot(const char* path) override;
    void Shutdown() override;
    bool IsReady() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace he::samples::cube02
