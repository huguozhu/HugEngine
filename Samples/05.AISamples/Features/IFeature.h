#pragma once

#include "Core/Types.h"
#include "Math/Math.h"

// ============================================================
// IFeature — 05.AISamples 功能模块统一接口
//
// 每个原 Sample（05~10）迁移为一个 Feature：
// 主程序持有共享的引擎/RHI/AI 设备/渲染管线/相机，
// Feature 只负责：场景搭建、CPU 逻辑（Update）、ImGui 面板（RenderUI）。
// 3D 场景（GetWorld/GetSceneGraph）由主程序用共享管线统一渲染。
// ============================================================

namespace he {
class World;
class SceneGraph;
} // namespace he

namespace he::rhi {
class IRHIDevice;
class IRHISwapChain;
} // namespace he::rhi

namespace he::ai {
class IAIDevice;
} // namespace he::ai

class IFeature {
public:
    virtual ~IFeature() = default;

    // Tab 标题（ImGui 面板名）
    virtual const char* GetName() const = 0;

    // 初始化：设备/AI 由主程序注入（不拥有）
    virtual bool Initialize(he::rhi::IRHIDevice* device, he::rhi::IRHISwapChain* sc,
                            he::ai::IAIDevice* ai) = 0;
    virtual void Shutdown() = 0;

    // 每帧 CPU 逻辑（智能体节律 / 动画 / 一次性推理）
    virtual void Update(float dt) = 0;

    // ImGui 面板（每帧调用，主循环 ImGui 窗口内）
    virtual void RenderUI() = 0;

    // --- 3D 场景（供主程序共享管线渲染；返回 nullptr = 不参与）---
    virtual he::World* GetWorld() { return nullptr; }
    virtual he::SceneGraph* GetSceneGraph() { return nullptr; }
    // 是否需要 3D 渲染（纯面板功能如 GPU 推理可返回 false）
    virtual bool NeedsRender3D() const { return true; }

    // 切换到此功能时的相机复位点（可选覆盖）
    virtual he::float3 GetDefaultCameraPos() const { return he::float3(0.0f, 3.0f, 8.0f); }
};
