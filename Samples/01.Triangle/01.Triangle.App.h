#pragma once

// ============================================================
// 01.Triangle —— 场景与渲染（桌面与鸿蒙共用同一份）
//
// 覆盖三条渲染路径（共用同一个旋转角）：
//   · 光栅化（VS/PS，红色三角形 + 推送常量旋转）
//   · Ray Tracing（RayGen 写离屏存储纹理 → 全屏拷贝）
//   · Mesh Shader（MS 直接输出三角形）
//
// 平台相关的东西已经全部移出本文件：
//   · 桌面宿主 = Platforms/Win32/DesktopHost.cpp（Main.Win32.cpp 只做配置）
//   · 鸿蒙宿主 = Platforms/OHOS/entry/src/main/cpp/AppMain.cpp（XComponent + napi）
// 两边编的是同一份 App 实现。
// ============================================================

#include "App/IApp.h"

#include <memory>

namespace he {
class Engine;
struct InputState;   // 注意是 struct（与 IInput.h 一致；class/struct 混用会让 MSVC 生成不同的修饰名）
}

namespace he::samples::triangle01 {

class App : public IApp, public IBackendSwitchApp {
public:
    using Options = IApp::Options;

    App();
    ~App() override;

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    /// 建设备 / 交换链 / PSO / RT / Mesh / ImGui（可重复调用：内部先 Shutdown）
    bool Initialize(Engine& engine, rhi::Backend backend) {
        return Initialize(engine, backend, Options{});
    }
    bool Initialize(Engine& engine, rhi::Backend backend, const Options& opt) override;

    /// 每帧：旋转角 + 渲染 + ImGui + Present。返回 false 表示本帧 Acquire 失败
    bool Frame(f32 deltaTime, const InputState& input) override;

    /// 交换链尺寸变化（旋转/分屏/拖拽窗口）
    void Resize(u32 width, u32 height) override;

    /// 请求截取下一帧到 path（PNG），用于"分享当前画面"
    void RequestScreenshot(const char* path) override;

    /// 释放全部 GPU 资源
    void Shutdown() override;

    bool IsReady() const override;

    /// 当前后端（宿主用它决定是否需要切换）
    rhi::Backend CurrentBackend() const override;

    /// 面板里选中的目标后端；宿主每帧取一次，非 Count 表示请求切换
    bool TakeBackendRequest(rhi::Backend& out) override;

    /// 渲染模式：0=光栅化 1=RT 2=Mesh（桌面自动化用）
    void SetRenderMode(int mode);

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace he::samples::triangle01
