#pragma once

// ============================================================
// 01.Triangle 的可移植子集 —— 与 `02.Cube.App` 同一套 App 接口
//
// 为什么要抽出来：鸿蒙真机只跑"三角形"这一子集（无 D3D 后端），
// 而桌面 01.Triangle 还要保留后端热切换 / RT / Mesh Shader。
// 把渲染与面板放进 App，桌面 main 与鸿蒙入口共用同一份代码，
// 改一处两边都生效，也能用 Windows 的 `01.Triangle.Subset` 做回归。
//
// 覆盖的渲染路径：
//   · 光栅化（VS/PS，红色三角形 + 推送常量旋转）
//   · Ray Tracing（RabGen 写离屏存储纹理 → 全屏拷贝）
//   · Mesh Shader（MS 直接输出三角形）
// 三条路径共用同一个旋转角。
// ============================================================

#include "Core/Core.h"
#include "RHI/RHI.h"

#include <memory>

namespace he {
class Engine;
struct InputState;   // 注意是 struct（与 IInput.h 一致；class/struct 混用会让 MSVC 生成不同的修饰名）
}

namespace he::samples::triangle01 {

class App {
public:
    /// 初始化选项（保持聚合体：调用方可用 `{ .enableImGui = false }`）
    struct Options {
        bool enableImGui = true;   ///< 是否创建 ImGui
    };

    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    /// 建设备 / 交换链 / PSO / RT / Mesh / ImGui（可重复调用：内部先 Shutdown）
    bool Initialize(Engine& engine, rhi::Backend backend) {
        return Initialize(engine, backend, Options{});
    }
    bool Initialize(Engine& engine, rhi::Backend backend, const Options& opt);

    /// 每帧：旋转角 + 渲染 + ImGui + Present。返回 false 表示本帧 Acquire 失败
    bool Frame(f32 deltaTime, const InputState& input);

    /// 交换链尺寸变化（旋转/分屏/拖拽窗口）
    void Resize(u32 width, u32 height);

    /// 请求截取下一帧到 path（PNG），用于"分享当前画面"
    void RequestScreenshot(const char* path);

    /// 释放全部 GPU 资源
    void Shutdown();

    bool IsReady() const;

    /// 当前后端（桌面 main 用它决定是否需要切换）
    rhi::Backend CurrentBackend() const;

    /// 面板里选中的目标后端；main 每帧取一次，非 Count 表示请求切换
    bool TakeBackendRequest(rhi::Backend& out);

    /// 渲染模式：0=光栅化 1=RT 2=Mesh（桌面自动化用）
    void SetRenderMode(int mode);

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace he::samples::triangle01
