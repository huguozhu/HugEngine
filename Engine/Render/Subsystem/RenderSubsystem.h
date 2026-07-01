#pragma once

#include "Core/Types.h"
#include "Pipeline/Camera.h"

// 前向声明 — 避免子系统头文件引入全量依赖
namespace rhi {
    class IRHIDevice;
    class IRHICommandList;
}

namespace he {
    class World;
    class SceneGraph;
}

namespace he::render {

class RenderGraphBuilder; // Frame Graph 桥梁（Phase 2）

// ============================================================================
// SubsystemContext — 渲染子系统每帧上下文
//
// 包含当前帧的场景快照，由管线在 Execute() 前组装。
// 子系统从 Context 读取所需数据，不持有所有权。
// ============================================================================
struct SubsystemContext {
    he::World*        world       = nullptr;  // ECS 世界（光源、网格体、组件）
    he::SceneGraph*   sceneGraph  = nullptr;  // 场景图（世界空间变换）
    const CameraData* camera      = nullptr;  // 当前帧相机
    u32               viewportWidth  = 0;     // 视口宽度
    u32               viewportHeight = 0;     // 视口高度
};

// ============================================================================
// IRenderSubsystem — 渲染子系统公共接口
//
// 设计要点：
//   1. 管线无关 — Shadow / GI / PostProcess 全部继承此接口
//   2. 双路径支持：
//      - Render()  命令式路径（Phase 1，直接录制 GPU 命令）
//      - Compile() 声明式路径（Phase 2，注册 Pass 到 Frame Graph）
//   3. 运行时可通过 SetEnabled() 开关子系统
// ============================================================================
class IRenderSubsystem {
    HE_DECLARE_NON_COPYABLE(IRenderSubsystem);

public:
    IRenderSubsystem() = default;
    virtual ~IRenderSubsystem() = default;

    // ---- 生命周期 ----

    /// 创建 GPU 资源（纹理 / 缓冲区 / PSO / 描述符集等）
    /// @param device   不持有所有权，生命周期由调用者保证
    /// @param width    初始视口宽度
    /// @param height   初始视口高度
    /// @return 初始化是否成功
    virtual bool Initialize(rhi::IRHIDevice* device,
                            u32 width, u32 height) = 0;

    /// 释放所有 GPU 资源，回到未初始化状态
    virtual void Shutdown() = 0;

    // ---- 每帧更新（CPU 侧：收集场景数据、检测脏状态） ----

    /// 每帧渲染前调用一次。从 Context 收集场景信息、更新脏数据。
    /// 逻辑上不操作 GPU，可在辅助线程执行。
    virtual void Update(const SubsystemContext& ctx) = 0;

    // ---- 路径 A：命令式渲染（Phase 1 使用） ----

    /// 直接录制 GPU 命令到 cmdList。
    /// 调用前必须先调用 Update()。
    virtual void Render(rhi::IRHICommandList* cmdList) = 0;

    // ---- 路径 B：声明式编译（Phase 2 Frame Graph 使用） ----

    /// 将本子系统的 Pass 和临时资源注册到 Frame Graph Builder。
    /// 基类默认空实现 — 子类按需重写即可获得自动 Barrier / 别名 / 裁剪。
    /// @param builder   Frame Graph 构造器（声明资源与 Pass）
    /// @param ctx       当前帧上下文
    virtual void Compile(RenderGraphBuilder& builder,
                         const SubsystemContext& ctx)
    {
        (void)builder; (void)ctx; // 默认不参与 Frame Graph
    }

    // ---- 着色器注入 ----

    /// 绑定本子系统的输出到当前命令缓冲，
    /// 使后续 Draw 调用可采样其计算结果。
    virtual void Bind(rhi::IRHICommandList* cmdList) const = 0;

    // ---- 窗口适配 ----

    /// 视口尺寸变更回调（交换链重建 / 窗口拉伸时触发）。
    /// 需要重建分辨率相关资源的子类必须重写。
    virtual void OnResize(u32 width, u32 height) = 0;

    // ---- 状态查询 ----

    [[nodiscard]] virtual const char* GetName()  const = 0;  // 子系统名称（调试 / 日志）
    [[nodiscard]] virtual bool        IsReady()  const = 0;  // GPU 资源是否就绪
    [[nodiscard]] virtual bool        IsEnabled() const = 0; // 子系统是否启用
    virtual void SetEnabled(bool enabled) = 0;

protected:
    rhi::IRHIDevice* m_Device = nullptr;  // RHI 设备（不持有所有权）
    u32 m_Width  = 0;  // 当前视口宽度
    u32 m_Height = 0;  // 当前视口高度
    bool m_Enabled = true;
};

} // namespace he::render
