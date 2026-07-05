#pragma once

#include "Pipeline/IRenderPipeline.h"
#include "Pipeline/Material.h"
#include "GI/GlobalIllumination.h"
#include "GI/GI_RSM.h"
#include "RHI/RHI.h"
#include "RenderGraph.h"
#include "Asset/BindlessTextureManager.h"

namespace he::render { class GI_IBL; class GI_RSM; }
namespace he::render { class ToneMapPass; class SkyboxPass; class SceneRenderer; }

#include "Shadow/IShadowSystem.h"
#include "SceneRenderer.h"
#include "Pipeline/ClusteredShading.h"
#include "Pipeline/GPUCulling.h"
#include "Pipeline/GPUScene.h"
#include "Pipeline/MeshBatcher.h"
#include "GI/GI_SSGI.h"
#include "PostProcess/SSAO.h"
#include "PostProcess/ToneMapPass.h"
#include "PostProcess/SkyboxPass.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "AntiAliasing/AntiAliasing.h"

#include <memory>
#include <vector>

namespace he::render {

// ============================================================================
// DeferredPipeline — 延迟渲染管线（GBuffer + Lighting Pass）
//
// 复用 ShadowSystem / GI_IBL / GI_RSM / ToneMapPass / SkyboxPass
// GBuffer 3×MRT (albedo+metallic / normal+roughness / emissive+ao) + D32
// Lighting Pass 全屏三角形 PBR + IBL + RSM + Shadow
// ============================================================================
class DeferredPipeline : public IRenderPipeline {
    HE_DECLARE_NON_COPYABLE(DeferredPipeline);

public:
    DeferredPipeline()  = default;
    ~DeferredPipeline() override = default;

    bool Initialize(rhi::IRHIDevice* device) override;
    void Shutdown() override;
    void NextFrame() override;
    void OnResize(u32 width, u32 height) override;
    const char* GetName() const override { return "DeferredPipeline"; }

    void Render(rhi::IRHICommandList* cmd, he::World& world,
                he::SceneGraph& sg, const CameraData& camera) override;

    IShadowSystem*       GetShadowSystem() override { return m_ShadowSystem.get(); }
    IGlobalIllumination* GetGI()           override { return m_GI.get(); }
    ToneMapPass*         GetToneMap()            { return m_ToneMap.get(); }
    ClusteredShading&    GetClusteredShading()   { return m_ClusteredShading; }
    GPUCulling&          GetGPUCulling()         { return m_GPUCulling; }
    void SetSwapChain(rhi::IRHISwapChain* sc)  { m_SwapChain = sc; }

    rhi::IRHIBuffer* GetCurrentObjectBuffer()  { return m_ObjectBuffers[m_CurrentFrameSlot].get(); }
    rhi::IRHIBuffer* GetCurrentShadowBuffer()  { return m_ShadowBuffers[m_CurrentFrameSlot].get(); }
    rhi::IRHIBuffer* GetCurrentShadowObjBuffer(){ return m_ShadowObjBuffers[m_CurrentFrameSlot].get(); }

private:
    void BuildFrameGraph(RenderGraph& rg, he::World& world,
                         he::SceneGraph& sg, const CameraData& camera);
    void CollectLights(PushConstantData& pc, he::World& world,
                       he::SceneGraph& sg, const CameraData& camera);
    void UpdateIBLBindings(GI_IBL* gi);
    void UpdateRSMBindings();

    rhi::IRHIDevice* m_Device = nullptr;
    rhi::IRHISwapChain* m_SwapChain = nullptr;

    // GBuffer (graph-managed, OnResize 重建)
    std::unique_ptr<rhi::IRHITexture> m_GBufferA, m_GBufferB, m_GBufferC;
    // GBuffer 第 4 张 MRT：velocity（RG16_FLOAT，UV 空间运动矢量）
    std::unique_ptr<rhi::IRHITexture> m_GBufferD;
    std::unique_ptr<rhi::IRHITexture> m_GBufferDepth;
    std::unique_ptr<rhi::IRHIPipelineState> m_GBufferPSO;
    rhi::DescriptorSetLayoutHandle m_GBufferLayout = rhi::kInvalidLayout;   // set=0: per-frame + bindless
    rhi::DescriptorSetHandle       m_GBufferSet    = rhi::kInvalidSet;      // set=0 共享（含 bindless）

    // Lighting PSO + 描述符
    std::unique_ptr<rhi::IRHIPipelineState> m_LightingPSO;
    rhi::DescriptorSetLayoutHandle m_LightingLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_LightingSet    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHITexture> m_BindlessPlaceholder;
    std::unique_ptr<rhi::IRHISampler> m_BindlessSampler;

    // HDR (Lighting 输出 + ToneMap 输入)
    std::unique_ptr<rhi::IRHITexture> m_HDRTarget, m_HDRDepth;
    std::unique_ptr<rhi::IRHISampler> m_HDRSampler;

    // 三缓冲
    std::unique_ptr<rhi::IRHIBuffer> m_LightBuffers[MAX_FRAMES_IN_FLIGHT];
    std::unique_ptr<rhi::IRHIBuffer> m_ObjectBuffers[MAX_FRAMES_IN_FLIGHT];
    std::unique_ptr<rhi::IRHIBuffer> m_ShadowBuffers[MAX_FRAMES_IN_FLIGHT];
    std::unique_ptr<rhi::IRHIBuffer> m_ShadowObjBuffers[MAX_FRAMES_IN_FLIGHT];
    u32 m_CurrentFrameSlot = 0;

    // 子系统
    std::unique_ptr<IShadowSystem>       m_ShadowSystem;
    std::unique_ptr<IGlobalIllumination> m_GI;
    std::unique_ptr<GI_RSM>              m_RSM;
    std::unique_ptr<ToneMapPass>         m_ToneMap;
    std::unique_ptr<SkyboxPass>          m_Skybox;
    std::unique_ptr<SceneRenderer>       m_SceneRenderer;

    // 时域抗锯齿
    std::unique_ptr<IAntiAliasing> m_AntiAliasing;

    // Clustered Shading
    ClusteredShading m_ClusteredShading;
    std::unique_ptr<rhi::IRHIBuffer> m_LightGridBuffer;       // binding 7
    std::unique_ptr<rhi::IRHIBuffer> m_LightIndexListBuffer;  // binding 8
    std::vector<GPULight> m_CachedLights;  // CPU 端缓存，避免 culling 时重复 Map

    // GPU Culling
    GPUCulling m_GPUCulling;
    GPUScene   m_GPUScene;
    MeshBatcher m_MeshBatcher;
    bool       m_BatchBuilt = false;

    // SSGI + SSAO
    GI_SSGI m_SSGI;
    SSAO    m_SSAO;
    std::vector<u32> m_GPUVisibleIndices;


    // 相机矩阵缓存（当前帧 + 上一帧，用于 velocity 计算和 TAA）
    float4x4 m_PrevViewProj = float4x4(1.0f);
    float4x4 m_CurrViewProj = float4x4(1.0f);

    u32 m_Width = 1920, m_Height = 1080;
    bool m_Ready = false;
};

} // namespace he::render
