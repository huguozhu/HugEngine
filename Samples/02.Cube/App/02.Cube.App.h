#pragma once

// ============================================================
// 02.Cube.App —— 02 样例的"可移植子集"（立方体 + 球体 + 天空盒 + 相机）
//
// 为什么单独抽一份 App：
//   原 Samples/02.Cube/02.Cube.cpp 是 1800+ 行的全功能演示（物理/导航/骨骼/粒子/贴花/
//   多种管线），其中的对象大量依赖桌面环境。鸿蒙首版只移植最核心的一条路径，
//   于是把"场景 + Forward 管线 + 相机 + 一帧的渲染"收进本类：
//     · Windows 入口（Main_Subset.cpp）与鸿蒙入口（Platforms/OHOS/.../AppMain.cpp）共用
//     · 平台差异只通过 Engine / IWindow / InputState 进入，这里不出现 GLFW/OHOS API
// ============================================================

#include "Core/Types.h"
#include "Platform/IInput.h"
#include "RHI/Types.h"   // rhi::Backend：枚举底层类型固定，不能前向声明

#include <memory>

namespace he {
class Engine;
}

namespace he::rhi {
class IRHIDevice;
}

namespace he::samples::cube02 {

class App {
public:
    /// 初始化选项（保持**聚合体**：调用方可用 `{ .enableImGui = false }` 指定初始化）
    struct Options {
        bool enableImGui = true;   ///< 是否创建 ImGui
    };

    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    /// 建设备 / 交换链 / 场景 / Forward 管线 / 命令列表（可重复调用：内部先 Shutdown）
    ///
    /// 【为什么是重载而不是默认实参】`const Options& = {}` 在类定义内解析时会依赖嵌套
    /// 类型的 default member initializer，clang 报 "default member initializer needed
    /// within definition of enclosing class"；给 Options 加构造函数又会破坏指定初始化。
    bool Initialize(Engine& engine, rhi::Backend backend) {
        return Initialize(engine, backend, Options{});
    }
    bool Initialize(Engine& engine, rhi::Backend backend, const Options& opt);

    /// 每帧：相机 + 渲染 + Present。返回 false 表示本帧 Acquire 失败（调用方 continue）
    bool Frame(f32 deltaTime, const InputState& input);

    /// 交换链尺寸变化（旋转/分屏/拖拽窗口）
    void Resize(u32 width, u32 height);

    /// 释放全部 GPU 资源（先于 Engine/Window 销毁调用）
    void Shutdown();

    bool IsReady() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace he::samples::cube02
