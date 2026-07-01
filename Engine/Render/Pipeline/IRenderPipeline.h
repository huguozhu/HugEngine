#pragma once

#include "Pipeline/Camera.h"
#include "Core/Types.h"

// 前向声明
namespace he { class World; class SceneGraph; }
namespace he::rhi { class IRHIDevice; class IRHICommandList; }
namespace he::render { class ShadowSystem; class IGlobalIllumination; }

namespace he::render {

// ============================================================================
// IRenderPipeline — 渲染管线抽象基类
//
// 定义所有渲染管线的公共接口：
//   1. 生命周期:  Initialize / Shutdown
//   2. 每帧编排:  NextFrame / Render
//   3. 窗口适配:  OnResize
//   4. 子系统访问: GetShadowSystem / GetGI（可选覆写）
//
// 继承树：
//   IRenderPipeline
//     ├── ForwardPipeline  前向 PBR 管线
//     ├── DeferredPipeline 延迟渲染管线（待实现）
//     └── ForwardPlusPipeline Forward+ 管线（待实现）
// ============================================================================
class IRenderPipeline {
public:
    virtual ~IRenderPipeline() = default;

    // ---- 生命周期 ----

    /// 创建 GPU 资源（PSO / 描述符集 / 纹理 / 缓冲区等）
    virtual bool Initialize(rhi::IRHIDevice* device) = 0;

    /// 释放所有 GPU 资源
    virtual void Shutdown() = 0;

    // ---- 每帧 ----

    /// 帧首推进三缓冲槽位，需在 Render 前调用
    virtual void NextFrame() = 0;

    /// 渲染完整一帧（场景 → 后处理 → 输出到 SwapChain）
    virtual void Render(rhi::IRHICommandList* cmd,
                        he::World& world, he::SceneGraph& sg,
                        const CameraData& camera) = 0;

    // ---- 窗口适配 ----

    /// 视口尺寸变更时调用（交换链重建 / 窗口拉伸）
    virtual void OnResize(u32 width, u32 height) = 0;

    // ---- 调试 ----

    /// 管线名称（日志 / 调试）
    virtual const char* GetName() const = 0;

    // ---- 子系统访问（可选覆写） ----

    /// 获取阴影子系统，无阴影时返回 nullptr
    virtual ShadowSystem* GetShadowSystem() { return nullptr; }

    /// 获取 GI 子系统，无 GI 时返回 nullptr
    virtual IGlobalIllumination* GetGI() { return nullptr; }
};

} // namespace he::render
