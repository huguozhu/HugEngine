#pragma once

// ============================================================
// 03.Sponza-Forward —— 场景与渲染（桌面与鸿蒙共用同一份）
//
// 鸿蒙侧跑"Sponza 场景 + 前向管线 + 天空盒 + 触摸相机"这一子集：
//   · 保留：glTF 场景、纹理（bindless）、方向光/补光、HDR 天空盒、CSM 阴影
//   · 待并入（路线1 阶段2）：Ray Tracing 路径、配置读写、渲染线程
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

namespace he::samples::sponza03 {

class App : public IApp {
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

    /// 每帧：相机 + 场景装配 + 前向渲染 + ToneMap + ImGui
    bool Frame(f32 deltaTime, const InputState& input) override;

    void Resize(u32 width, u32 height) override;

    /// 请求截取下一帧到 path（PNG），用于"分享当前画面"
    void RequestScreenshot(const char* path) override;
    void Shutdown() override;
    bool IsReady() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace he::samples::sponza03
