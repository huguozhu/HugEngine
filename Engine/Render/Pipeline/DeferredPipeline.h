#pragma once

#include "Pipeline/IRenderPipeline.h"
#include "Pipeline/Material.h"
#include "GI/GlobalIllumination.h"
#include "GI/GI_RSM.h"
#include "RHI/RHI.h"
#include "RenderGraph.h"

namespace he::render { class GI_IBL; class GI_RSM; }
namespace he::render { class ToneMapPass; class SkyboxPass; class SceneRenderer; }

#include "Shadow/IShadowSystem.h"
#include "SceneRenderer.h"
#include "PostProcess/ToneMapPass.h"
#include "PostProcess/SkyboxPass.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"

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
    void SetSwapChain(rhi::IRHISwapChain* sc)  { m_SwapChain = sc; }

    rhi::IRHIBuffer* GetCurrentObjectBuffer()  { return m_ObjectBuffers[m_CurrentFrameSlot].get(); }
    rhi::IRHIBuffer* GetCurrentShadowBuffer()  { return m_ShadowBuffers[m_CurrentFrameSlot].get(); }
    rhi::IRHIBuffer* GetCurrentShadowObjBuffer(){ return m_ShadowObjBuffers[m_CurrentFrameSlot].get(); }

    // 为每个 mesh 创建独立描述符集（set=1: 仅纹理绑定 5-8，静态永不变）
    rhi::DescriptorSetHandle CreateTextureDescriptorSet(
        rhi::IRHITexture* baseColor, rhi::IRHISampler* bcSampler,
        rhi::IRHITexture* normal,   rhi::IRHISampler* nSampler,
        rhi::IRHITexture* metallicRoughness, rhi::IRHISampler* mrSampler,
        rhi::IRHITexture* occlusion, rhi::IRHISampler* ocSampler);

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
    std::unique_ptr<rhi::IRHITexture> m_GBufferDepth;
    std::unique_ptr<rhi::IRHIPipelineState> m_GBufferPSO;
    rhi::DescriptorSetLayoutHandle m_GBufferLayout = rhi::kInvalidLayout;   // set=0: per-frame
    rhi::DescriptorSetLayoutHandle m_PerMeshLayout  = rhi::kInvalidLayout;  // set=1: per-mesh 纹理
    rhi::DescriptorSetHandle       m_GBufferSet    = rhi::kInvalidSet;      // set=0 共享

    // Lighting PSO + 描述符
    std::unique_ptr<rhi::IRHIPipelineState> m_LightingPSO;
    rhi::DescriptorSetLayoutHandle m_LightingLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_LightingSet    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHITexture> m_PlaceholderTex;
    std::unique_ptr<rhi::IRHISampler> m_PlaceholderSamp;

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

    u32 m_Width = 1920, m_Height = 1080;
    bool m_Ready = false;
};

} // namespace he::render
