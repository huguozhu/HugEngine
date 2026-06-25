#pragma once

#include "Render/Pipeline/Material.h"
#include "Render/Pipeline/Camera.h"
#include "RHI/RHI.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/MeshComponent.h"
#include "Scene/Transform.h"
#include "Core/Types.h"

#include <memory>

// ============================================================
// ForwardPipeline — 前向 PBR 渲染管线
//
// Phase 2 B1: 单 Pass PBR + 内联 ToneMapping
// 后续版本: HDR 中间目标 + 分离 ToneMapping Pass
// ============================================================

namespace he::render {

class ForwardPipeline {
    HE_DECLARE_NON_COPYABLE(ForwardPipeline);

public:
    ForwardPipeline();
    ~ForwardPipeline();

    // 初始化：创建 PSO、着色器模块
    void Initialize(rhi::IRHIDevice* device);
    void Shutdown();

    // 每帧开始（设置视口/裁剪等）
    void BeginFrame(rhi::IRHICommandList* cmd, u32 width, u32 height);

    // 渲染场景中所有 MeshComponent 实体
    void RenderScene(
        rhi::IRHICommandList* cmd,
        he::World& world,
        he::SceneGraph& sceneGraph,
        const CameraData& camera,
        const PushConstantData& lighting);

    // 每帧结束
    void EndFrame(rhi::IRHICommandList* cmd);

    // 获取管线状态对象（用于 CommandList::SetPipeline）
    rhi::IRHIPipelineState* GetPipelineState() const { return m_PBR_PSO.get(); }

    // 便捷：填充默认的光照 push constant 数据
    static PushConstantData MakeDefaultLighting();

private:
    // 绘制单个网格
    void DrawMesh(
        rhi::IRHICommandList* cmd,
        he::MeshComponent* mesh,
        const float4x4& worldMatrix,
        const float4x4& viewProjMatrix,
        const PBRMaterial& material,
        const CameraData& camera,
        const PushConstantData& lighting);

    rhi::IRHIDevice* m_Device = nullptr;

    // PBR 管线状态
    std::unique_ptr<rhi::IRHIPipelineState> m_PBR_PSO;

    // 着色器字节码
    rhi::ShaderBytecode m_VS;
    rhi::ShaderBytecode m_FS;
};

} // namespace he::render
