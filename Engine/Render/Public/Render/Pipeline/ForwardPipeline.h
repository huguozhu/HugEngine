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
#include <vector>

// ============================================================
// ForwardPipeline — 前向 PBR 渲染管线（含阴影通道）
// ============================================================

namespace he::render {

// 阴影通道 Push Constant
struct alignas(16) ShadowPushConstant {
    float4x4 lightViewProj;   // 光照裁剪矩阵
    u32      objectIndex;     // GPU 对象索引
    u32      _pad[3];         // 对齐到 80 字节
};
static_assert(sizeof(ShadowPushConstant) == 80, "ShadowPushConstant must be 80 bytes");

class ForwardPipeline {
    HE_DECLARE_NON_COPYABLE(ForwardPipeline);

public:
    ForwardPipeline();
    ~ForwardPipeline();

    void Initialize(rhi::IRHIDevice* device);
    void Shutdown();

    void BeginFrame(rhi::IRHICommandList* cmd, u32 width, u32 height);

    // 渲染场景（含阴影通道）
    void RenderScene(
        rhi::IRHICommandList* cmd,
        he::World& world,
        he::SceneGraph& sceneGraph,
        const CameraData& camera);

    void EndFrame(rhi::IRHICommandList* cmd);

    rhi::IRHIPipelineState* GetPipelineState() const { return m_PBR_PSO.get(); }

    // 绑定基础色纹理（外部加载后调用）
    void SetBaseColorTexture(rhi::IRHITexture* tex, rhi::IRHISampler* sampler);

private:
    // 阴影通道 — 渲染所有投射阴影的光源的深度贴图
    void RenderShadowPass(
        rhi::IRHICommandList* cmd,
        he::World& world,
        he::SceneGraph& sg,
        const std::vector<const he::LightComponent*>& shadowLights,
        const std::vector<GPUShadowData>& shadowGPUData);

    // 从 World 收集活跃光源，填充 PushConstant 的光照字段
    void CollectLights(PushConstantData& pc,
                       std::vector<GPUShadowData>& shadowData,
                       he::World& world, he::SceneGraph& sg);

    // 绘制单个网格（主管线）
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
    std::unique_ptr<rhi::IRHIPipelineState> m_ShadowPSO;  // 阴影深度专用 PSO

    // 主管道 Descriptor Set + Storage Buffers（光照 + 对象数据 + 阴影数据 + 阴影贴图）
    rhi::DescriptorSetLayoutHandle m_DescLayout     = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_DescSet        = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIBuffer> m_LightBuffer;
    std::unique_ptr<rhi::IRHIBuffer> m_ObjectBuffer;   // GPUObjectData[MAX_OBJECTS]
    std::unique_ptr<rhi::IRHIBuffer> m_ShadowBuffer;   // GPUShadowData[MAX_SHADOWS]

    // 阴影贴图 + 采样器
    std::unique_ptr<rhi::IRHITexture>  m_ShadowMap;          // 深度纹理（待阴影通道渲染后使用）
    std::unique_ptr<rhi::IRHITexture>  m_ShadowPlaceholderTex; // 占位纹理（布局正确的 dummy）
    std::unique_ptr<rhi::IRHISampler>  m_ShadowSampler;      // PCF 比较采样器（用于正式阴影贴图）
    std::unique_ptr<rhi::IRHISampler>  m_ShadowPlaceholderSampler; // 占位纹理的普通采样器
    u32 m_ShadowMapSize = 2048;

    // 基础色纹理（默认 1×1 白色，运行时替换为 glTF 纹理）
    std::unique_ptr<rhi::IRHITexture>  m_DefaultBaseColorTex;
    std::unique_ptr<rhi::IRHISampler>  m_DefaultBaseColorSampler;

    // 着色器字节码
    rhi::ShaderBytecode m_VS;
    rhi::ShaderBytecode m_FS;
    rhi::ShaderBytecode m_ShadowVS;
    rhi::ShaderBytecode m_ShadowFS;
};

} // namespace he::render
