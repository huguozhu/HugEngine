#pragma once

#include "Pipeline/Camera.h"
#include "Core/Types.h"
// 第③段第 4/5 批：帧入口收快照 ⇒ 接口要暴露快照、装配器与网格注册表（样例经接口切换管线时用）
#include "Threading/FrameSceneSnapshot.h"
#include "Threading/FrameSnapshotAssembler.h"
#include "Threading/MeshRegistry.h"

// 前向声明
namespace he { class World; class SceneGraph; }
namespace he::rhi { class IRHIDevice; class IRHICommandList; class IRHISwapChain; }
namespace he::render { class IShadowSystem; class IGlobalIllumination; struct GIConfig; }

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
    /// width/height：**初始视口尺寸**（像素）；传 0 = 沿用默认尺寸
    /// （rhi::kDefaultBackBufferWidth/Height）。
    ///
    /// 为什么必须能传尺寸：管线内所有尺寸相关资源（GBuffer / HDR / 后处理 /
    /// GI 半分辨率纹理 / RT 输出）都在 Initialize 里按此尺寸创建。若这里用默认
    /// 尺寸而真实交换链尺寸不同（例：请求窗口 1920x1080，客户区实际 1920x1061），
    /// 紧随其后的 OnResize 会把整套资源销毁重建——实测这会在启动期造成纹理/
    /// framebuffer churn：校验层报大量 "command buffer ... objects bound ...
    /// were invalidated"，并给"已销毁纹理仍被引用"留下窗口（偶发访问违例）。
    virtual bool Initialize(rhi::IRHIDevice* device, u32 width = 0, u32 height = 0) = 0;

    /// 释放所有 GPU 资源
    virtual void Shutdown() = 0;

    // ---- 每帧 ----

    /// 帧首推进三缓冲槽位，需在 Render 前调用
    virtual void NextFrame() = 0;

    /// 渲染完整一帧（场景 → 后处理 → 输出到 SwapChain）
    /// 【阶段 1 §15.1 第③段：帧入口收**快照**】不再收 `World&`/`SceneGraph&` —— 渲染输入由样例
    /// 在游戏线程用 `GetFrameAssembler().AssembleScene(...)` 装配好，管线只消费它。
    /// 调用顺序契约：`NextFrame()` → `AssembleScene` →（Forward/带阴影的路径）阴影收集 →
    /// `ResolveLightShadowIndices` → `Render(cmd, GetFrameSnapshot(), camera, dt)`。
    virtual void Render(rhi::IRHICommandList* cmd,
                        const FrameSceneSnapshot& snapshot,
                        const CameraData& camera,
                        float deltaTime = 0.016f) = 0;

    // ---- 快照交接（阶段 1 §15.1 第③段）----

    /// 本帧快照装配器（**游戏线程**用；读世界的代码在 `Threading/` 白名单层，管线只配置口径）
    virtual FrameSnapshotAssembler& GetFrameAssembler() = 0;
    /// 本帧快照（`AssembleScene` 之后有效；交给 `Render` 的就是它）
    virtual const FrameSceneSnapshot& GetFrameSnapshot() const = 0;
    /// 网格注册表（阴影技术 / RTPass 等按 `meshIndex` 取几何）
    virtual const MeshRegistry& GetMeshRegistry() const = 0;

    // ---- 窗口适配 ----

    /// 视口尺寸变更时调用（交换链重建 / 窗口拉伸）
    virtual void OnResize(u32 width, u32 height) = 0;

    /// 设置输出交换链（各管线覆写；用于运行时切换管线）
    virtual void SetSwapChain(rhi::IRHISwapChain* swapChain) { (void)swapChain; }

    // ---- 调试 ----

    /// 管线名称（日志 / 调试）
    virtual const char* GetName() const = 0;

    // ---- 子系统访问（可选覆写） ----

    /// 获取阴影子系统，无阴影时返回 nullptr
    virtual IShadowSystem* GetShadowSystem() { return nullptr; }

    /// 获取 GI 子系统，无 GI 时返回 nullptr
    virtual IGlobalIllumination* GetGI() { return nullptr; }

    /// 获取该管线的 GI 通道配置（各管线独立——可用通道子集不同）
    virtual GIConfig* GetGIConfig() { return nullptr; }

    /// 该管线支持的 GI 通道能力位（PipelineCaps::Forward / PipelineCaps::Deferred）
    virtual u32 GetGIPipelineCaps() const { return 0; }

    // ---- Shader 热重载 ----

    /// 热重载单个 Shader（传入新编译的 SPIR-V 字节码）
    /// @param shaderName 文件名（不含路径和扩展名，如 "PBR.frag"）
    /// @param newSpirv   新编译的 SPIR-V 字节码
    /// @return 受影响并已替换的 PSO 数量，0=未找到匹配, -1=失败
    virtual int ReloadShader(StringView shaderName,
                             const std::vector<u32>& newSpirv) { return -1; }
};

} // namespace he::render
