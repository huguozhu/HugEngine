#pragma once

// ============================================================
// 03.Sponza-Forward 的可移植子集 —— 与 01/02 的 App 同一套接口
//
// 鸿蒙侧只跑"Sponza 场景 + 前向管线 + 天空盒 + 触摸相机"这一子集：
//   · 保留：glTF 场景、纹理（bindless）、方向光/补光/点光、HDR 天空盒、CSM 阴影
//   · 暂不移植：Ray Tracing 路径、配置读写、桌面版那一整套面板开关
// 桌面用 `03.Sponza.Subset` 跑同一份代码做回归；完整功能仍在 `03.Sponza-Forward`。
// ============================================================

#include "Core/Core.h"
#include "RHI/RHI.h"

#include <memory>

namespace he {
class Engine;
struct InputState;
}

namespace he::samples::sponza03 {

class App {
public:
    /// 初始化选项（保持聚合体：可用 `{ .enableImGui = false }`）
    struct Options {
        bool enableImGui = true;   ///< 是否创建 ImGui
    };

    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    bool Initialize(Engine& engine, rhi::Backend backend) {
        return Initialize(engine, backend, Options{});
    }
    bool Initialize(Engine& engine, rhi::Backend backend, const Options& opt);

    /// 每帧：相机 + 场景装配 + 前向渲染 + ToneMap + ImGui
    bool Frame(f32 deltaTime, const InputState& input);

    void Resize(u32 width, u32 height);

    /// 请求截取下一帧到 path（PNG），用于"分享当前画面"
    void RequestScreenshot(const char* path);
    void Shutdown();
    bool IsReady() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace he::samples::sponza03
