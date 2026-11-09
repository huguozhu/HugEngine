#pragma once

// ============================================================
// 04.Sponza-Deferred 的可移植子集 —— 与 01/02/03 的 App 同一套接口
//
// 鸿蒙侧跑"Sponza + 延迟管线（GBuffer + 全屏 Lighting + ToneMap）+ 触摸相机"：
//   · 保留：glTF 场景、纹理（bindless）、方向光/补光、HDR 天空盒、CSM 阴影、GBuffer
//   · 为移动端性能关掉：SSGI / DDGI / SSR / SSAO / Clustered（桌面版仍可在完整样例里开）
// 桌面用 `04.Deferred.Subset` 跑同一份代码做回归。
// ============================================================

#include "Core/Core.h"
#include "RHI/RHI.h"

#include <memory>

namespace he {
class Engine;
struct InputState;
}

namespace he::samples::sponza04 {

class App {
public:
    /// 初始化选项（保持聚合体：可用 `{ .enableImGui = false }`）
    struct Options {
        bool enableImGui = true;
    };

    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    bool Initialize(Engine& engine, rhi::Backend backend) {
        return Initialize(engine, backend, Options{});
    }
    bool Initialize(Engine& engine, rhi::Backend backend, const Options& opt);

    /// 每帧：相机 + 场景装配 + 延迟渲染（GBuffer/Lighting/ToneMap）+ ImGui
    bool Frame(f32 deltaTime, const InputState& input);

    void Resize(u32 width, u32 height);
    void Shutdown();
    bool IsReady() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace he::samples::sponza04
