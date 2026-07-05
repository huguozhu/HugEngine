#pragma once

#include "Pipeline/IRenderPipeline.h"
#include "Pipeline/Material.h"
#include "GI/GlobalIllumination.h"
#include "RHI/RHI.h"
#include "RenderGraph.h"
#include "Asset/BindlessTextureManager.h"
#include "Pipeline/GPUCulling.h"
#include "AntiAliasing/AntiAliasing.h"

namespace he::render { class GI_IBL; }
namespace he::render { class GI_RSM; }
namespace he::render { class ToneMapPass; }
namespace he::render { class SkyboxPass; }
namespace he::render { class SceneRenderer; }

#include "Shadow/IShadowSystem.h"

#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/MeshComponent.h"
#include "Scene/LightComponent.h"
#include "Scene/Transform.h"
#include "Core/Types.h"

#include <memory>
#include <vector>

namespace he::render {

class ForwardPipeline : public IRenderPipeline {
    HE_DECLARE_NON_COPYABLE(ForwardPipeline);

public:
    ForwardPipeline();
    ~ForwardPipeline() override;

    // IRenderPipeline
    bool Initialize(rhi::IRHIDevice* device) override;
    void Shutdown() override;
    void NextFrame() override;
    void OnResize(u32 width, u32 height) override;
    const char* GetName() const override { return "ForwardPipeline"; }
    void Render(rhi::IRHICommandList* cmd, he::World& world,
                he::SceneGraph& sg, const CameraData& camera) override;

    // 子系统访问
    IShadowSystem*       GetShadowSystem() override { return m_ShadowSystem.get(); }
    IGlobalIllumination* GetGI()           override { return m_GI.get(); }
    ToneMapPass*         GetToneMap()            { return m_ToneMap.get(); }
    SkyboxPass*          GetSkybox()             { return m_Skybox.get(); }

    // ForwardPipeline 特有方法（命令式，保留兼容）
    void BeginFrame(rhi::IRHICommandList* cmd, u32 width, u32 height);
    void RenderScene(rhi::IRHICommandList* cmd, he::World& world,
                     he::SceneGraph& sg, const CameraData& camera);
    void EndFrame(rhi::IRHICommandList* cmd);

    // RenderGraph 模式（声明式 Pass 编排，自动 Barrier）
    void BuildFrameGraph(RenderGraph& rg, he::World& world, he::SceneGraph& sg,
                         const CameraData& camera);
    bool UseRenderGraph() const { return m_UseRenderGraph; }
    void SetUseRenderGraph(bool use) { m_UseRenderGraph = use; }
    void SetSwapChain(rhi::IRHISwapChain* sc) { m_SwapChain = sc; }
    rhi::IRHIPipelineState* GetPipelineState() const { return m_PBR_PSO.get(); }

    void SetMultiThreadedRecording(bool e) { m_MultiThreadRecord = e; }
    bool IsMultiThreadedRecording() const { return m_MultiThreadRecord; }

    // HDR 离屏渲染
    void BeginHDRPass(rhi::IRHICommandList* cmd, u32 w, u32 h);
    void EndHDRPass(rhi::IRHICommandList* cmd);
    void ResizeHDRTarget(u32 w, u32 h);

    void SetGI(std::unique_ptr<IGlobalIllumination> gi) { m_GI = std::move(gi); }
    IAntiAliasing* GetAntiAliasing() { return m_AntiAliasing.get(); }
    void SetAntiAliasing(std::unique_ptr<IAntiAliasing> aa) { m_AntiAliasing = std::move(aa); }
    void PrepareGI(rhi::IRHICommandList* cmd, he::World& world, he::SceneGraph& sg);
    GI_RSM* GetRSM() { return m_RSM.get(); }

    // 后处理（委托给子系统）
    void RenderToneMapPass(rhi::IRHICommandList* cmd);
    void RenderSkybox(rhi::IRHICommandList* cmd, he::World& world, const CameraData& camera);

    rhi::IRHIBuffer*         GetCurrentObjectBuffer() { return m_ObjectBuffers[m_CurrentFrameSlot].get(); }
    rhi::IRHIBuffer*         GetCurrentShadowBuffer() { return m_ShadowBuffers[m_CurrentFrameSlot].get(); }
    rhi::DescriptorSetHandle GetCurrentDescSet()      { return m_DescSets[m_CurrentFrameSlot]; }
    // 阴影专用 Object Buffer（独立于场景 Object Buffer，避免 CPU 录制时覆盖）
    rhi::IRHIBuffer*         GetCurrentShadowObjectBuffer() { return m_ShadowObjBuffers[m_CurrentFrameSlot].get(); }

    // HDR 纹理访问（供 ToneMapPass 使用）
    rhi::IRHITexture* GetHDRTarget()  const { return m_HDRTarget.get(); }
    rhi::IRHISampler* GetHDRSampler() const { return m_HDRSampler.get(); }
    GPUCulling& GetGPUCulling() { return m_GPUCulling; }
    u32 GetLastDrawCount() const { return m_LastDrawCount; }
    u32 GetLastTriCount()  const { return m_LastTriCount; }

private:
    void CollectLights(PushConstantData& pc, he::World& world, he::SceneGraph& sg, const CameraData& camera);
    void DrawMesh(rhi::IRHICommandList* cmd, he::MeshComponent* mesh,
                  const float4x4& worldMatrix, const float4x4& viewProjMatrix,
                  const PBRMaterial& material, const CameraData& camera,
                  const PushConstantData& lighting);
    void UploadLightBuffer();
    void UpdateIBLBindings(GI_IBL* gi);
    void UpdateRSMBindings();

    rhi::IRHIDevice* m_Device = nullptr;
    std::unique_ptr<rhi::IRHIPipelineState> m_PBR_PSO;

    rhi::DescriptorSetLayoutHandle m_PerFrameLayout = rhi::kInvalidLayout;  // set=0: per-frame + bindless
    rhi::DescriptorSetHandle       m_DescSets[MAX_FRAMES_IN_FLIGHT] = {};   // set=0 三缓冲
    std::unique_ptr<rhi::IRHIBuffer> m_LightBuffers[MAX_FRAMES_IN_FLIGHT];
    std::unique_ptr<rhi::IRHIBuffer> m_ObjectBuffers[MAX_FRAMES_IN_FLIGHT];
    std::unique_ptr<rhi::IRHIBuffer> m_ShadowBuffers[MAX_FRAMES_IN_FLIGHT];
    std::unique_ptr<rhi::IRHIBuffer> m_ShadowObjBuffers[MAX_FRAMES_IN_FLIGHT];  // 阴影专用 Object Buffer
    u32 m_CurrentFrameSlot = 0;

    // HDR 离屏渲染
    std::unique_ptr<rhi::IRHITexture> m_HDRTarget, m_HDRDepth;
    std::unique_ptr<rhi::IRHISampler> m_HDRSampler;
    u32 m_HDRWidth = 1920, m_HDRHeight = 1080;

    // Bindless 占位纹理/采样器
    std::unique_ptr<rhi::IRHITexture> m_BindlessPlaceholder;
    std::unique_ptr<rhi::IRHISampler> m_BindlessSampler;

    // 多线程录制（Sec CB 模循环复用，无池重置 VUID 问题）
    bool m_MultiThreadRecord = true;
    static constexpr u32 kMaxSecRecordLists = 8;
    // RenderGraph 模式（默认关闭，渐进迁移到声明式编排）
    bool m_UseRenderGraph = false;
    std::vector<std::unique_ptr<rhi::IRHICommandList>> m_SecRecordLists;

    // 着色器
    rhi::ShaderBytecode m_VS, m_FS;

    // 子系统
    std::unique_ptr<IGlobalIllumination> m_GI;
    std::unique_ptr<GI_RSM>              m_RSM;
    std::unique_ptr<IShadowSystem>       m_ShadowSystem;
    std::unique_ptr<IAntiAliasing>       m_AntiAliasing;
    rhi::IRHISwapChain* m_SwapChain = nullptr;
    std::unique_ptr<ToneMapPass>         m_ToneMap;
    std::unique_ptr<SkyboxPass>          m_Skybox;
    std::unique_ptr<SceneRenderer>       m_SceneRenderer;

    // GPU Culling
    GPUCulling m_GPUCulling;
    std::vector<u32> m_GPUVisibleIndices;
    u32 m_LastDrawCount = 0;
    u32 m_LastTriCount  = 0;  // GPU 剔除后的可见物体索引
};

} // namespace he::render
