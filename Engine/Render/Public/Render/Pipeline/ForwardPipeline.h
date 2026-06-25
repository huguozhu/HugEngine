#pragma once

#include "Render/Pipeline/Material.h"
#include "Render/Pipeline/Camera.h"
#include "RHI/RHI.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/MeshComponent.h"
#include "Scene/LightComponent.h"
#include "Scene/Transform.h"
#include "Core/Types.h"

#include <memory>

// ============================================================
// ForwardPipeline — 前向 PBR 渲染管线
// ============================================================

namespace he::render {

class ForwardPipeline {
    HE_DECLARE_NON_COPYABLE(ForwardPipeline);

public:
    ForwardPipeline();
    ~ForwardPipeline();

    void Initialize(rhi::IRHIDevice* device);
    void Shutdown();

    void BeginFrame(rhi::IRHICommandList* cmd, u32 width, u32 height);

    // 渲染场景
    void RenderScene(
        rhi::IRHICommandList* cmd,
        he::World& world,
        he::SceneGraph& sceneGraph,
        const CameraData& camera);

    void EndFrame(rhi::IRHICommandList* cmd);

    rhi::IRHIPipelineState* GetPipelineState() const { return m_PBR_PSO.get(); }

private:
    // 从 World 收集活跃光源，填充 PushConstant 的光照字段
    void CollectLights(PushConstantData& pc, he::World& world, he::SceneGraph& sg);

    // 绘制单个网格
    void DrawMesh(
        rhi::IRHICommandList* cmd,
        he::MeshComponent* mesh,
        const float4x4& worldMatrix,
        const float4x4& viewProjMatrix,
        const PBRMaterial& material,
        const CameraData& camera,
        const PushConstantData& lighting);

    // 将 CPU 端 GPULight 数组上传到 Storage Buffer（每帧调用）
    void UploadLightBuffer();

    rhi::IRHIDevice* m_Device = nullptr;
    std::unique_ptr<rhi::IRHIPipelineState> m_PBR_PSO;

    // Descriptor Set + Storage Buffer（多光源）
    rhi::DescriptorSetLayoutHandle m_LightDescLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_LightDescSet    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIBuffer> m_LightBuffer;

    // 着色器字节码
    rhi::ShaderBytecode m_VS;
    rhi::ShaderBytecode m_FS;
};

} // namespace he::render
