#pragma once

#include "Pipeline/IRenderPipeline.h"
#include "Pipeline/Material.h"
#include "GI/GlobalIllumination.h"
#include "RHI/RHI.h"

namespace he::render { class GI_IBL; }        // 前向声明 — 避免循环依赖

#include "Shadow/IShadowSystem.h"  // 完整类型 — 调用方通过 GetShadowSystem() 访问

#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/MeshComponent.h"
#include "Scene/LightComponent.h"
#include "Scene/Transform.h"
#include "Core/Types.h"

#include <memory>
#include <vector>

// ============================================================
// ForwardPipeline — 前向 PBR 渲染管线（阴影已抽取为 ShadowSystem）
// ============================================================

namespace he::render {

class ForwardPipeline : public IRenderPipeline {
    HE_DECLARE_NON_COPYABLE(ForwardPipeline);

public:
    ForwardPipeline();
    ~ForwardPipeline() override;

    // ---- IRenderPipeline 接口 ----
    bool Initialize(rhi::IRHIDevice* device) override;
    void Shutdown() override;
    void NextFrame() override;
    void OnResize(u32 width, u32 height) override;
    const char* GetName() const override { return "ForwardPipeline"; }

    /// 渲染完整一帧（HDR 离屏 → 场景 → 天空盒 → ToneMap 后处理）
    void Render(rhi::IRHICommandList* cmd, he::World& world,
                he::SceneGraph& sg, const CameraData& camera) override;

    // 子系统访问
    IShadowSystem*       GetShadowSystem() override { return m_ShadowSystem.get(); }
    IGlobalIllumination* GetGI()           override { return m_GI.get(); }

    // ---- ForwardPipeline 特有方法 ----

    void BeginFrame(rhi::IRHICommandList* cmd, u32 width, u32 height);

    // 渲染场景（光照 + PBR 绘制 + 视锥剔除）
    void RenderScene(
        rhi::IRHICommandList* cmd,
        he::World& world,
        he::SceneGraph& sceneGraph,
        const CameraData& camera);

    void EndFrame(rhi::IRHICommandList* cmd);

    rhi::IRHIPipelineState* GetPipelineState() const { return m_PBR_PSO.get(); }

    // 为指定纹理组合分配独立描述符集（渲染时直接绑定，避免 per-draw update）
    rhi::DescriptorSetHandle CreateTextureDescriptorSet(
        rhi::IRHITexture* baseColor, rhi::IRHISampler* bcSampler,
        rhi::IRHITexture* normal,   rhi::IRHISampler* nSampler,
        rhi::IRHITexture* metallicRoughness, rhi::IRHISampler* mrSampler,
        rhi::IRHITexture* occlusion, rhi::IRHISampler* ocSampler);

    // Phase 5-4: 多线程命令录制开关（ImGui 面板运行时切换）
    void SetMultiThreadedRecording(bool enable) { m_MultiThreadRecord = enable; }
    bool IsMultiThreadedRecording() const { return m_MultiThreadRecord; }

    // --- HDR 离屏渲染 ---
    void BeginHDRPass(rhi::IRHICommandList* cmd, u32 width, u32 height);
    void EndHDRPass(rhi::IRHICommandList* cmd);
    void RenderToneMapPass(rhi::IRHICommandList* cmd);
    void ResizeHDRTarget(u32 width, u32 height);

    // --- 子系统和访问器 ---

    // 渲染天空盒（遍历 Scene 中的 SkyboxComponent）
    void RenderSkybox(rhi::IRHICommandList* cmd, he::World& world,
                      const CameraData& camera);

    void SetGI(std::unique_ptr<IGlobalIllumination> gi) { m_GI = std::move(gi); }

    // GI 准备（Scene 渲染前调用：检测 Skybox → 更新 IBL 贴图）
    void PrepareGI(rhi::IRHICommandList* cmd, he::World& world);


    // 当前帧 Object/Shadow Buffer + 描述符集（供 ShadowSystem 复用）
    rhi::IRHIBuffer*           GetCurrentObjectBuffer() { return m_ObjectBuffers[m_CurrentFrameSlot].get(); }
    rhi::IRHIBuffer*           GetCurrentShadowBuffer() { return m_ShadowBuffers[m_CurrentFrameSlot].get(); }
    rhi::DescriptorSetHandle   GetCurrentDescSet()      { return m_DescSets[m_CurrentFrameSlot]; }

private:

    // 从 World 收集活跃光源，填充 PushConstant + Light SSBO
    void CollectLights(PushConstantData& pc,
                       he::World& world, he::SceneGraph& sg,
                       const CameraData& camera);

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

    // 主管道 Descriptor Set + Storage Buffers（光照 + 对象数据）
    rhi::DescriptorSetLayoutHandle m_DescLayout      = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_DescSets[MAX_FRAMES_IN_FLIGHT] = {};  // 三缓冲共享描述符集
    // 三缓冲 StorageBuffer（Phase 1 多线程渲染：CPU 写入当前帧，GPU 读取上一帧）
    std::unique_ptr<rhi::IRHIBuffer> m_LightBuffers[MAX_FRAMES_IN_FLIGHT];
    std::unique_ptr<rhi::IRHIBuffer> m_ObjectBuffers[MAX_FRAMES_IN_FLIGHT];  // GPUObjectData[MAX_OBJECTS]
    std::unique_ptr<rhi::IRHIBuffer> m_ShadowBuffers[MAX_FRAMES_IN_FLIGHT];  // GPUShadowData[MAX_SHADOWS]
    u32 m_CurrentFrameSlot = 0;  // 当前帧槽位 [0, MAX_FRAMES_IN_FLIGHT)
    // 所有已分配的 per-mesh 描述符集（用于每帧更新动态绑定 1-3）
    std::vector<rhi::DescriptorSetHandle> m_AllPerMeshDescSets;

    // HDR 离屏渲染（RGBA16_FLOAT）
    std::unique_ptr<rhi::IRHITexture>  m_HDRTarget;     // 离屏颜色纹理
    std::unique_ptr<rhi::IRHITexture>  m_HDRDepth;      // 离屏深度纹理
    std::unique_ptr<rhi::IRHISampler>  m_HDRSampler;    // 线性采样器
    u32 m_HDRWidth = 1920, m_HDRHeight = 1080;

    // ToneMap 全屏后处理（HDR → ACES → sRGB → SwapChain）
    std::unique_ptr<rhi::IRHIPipelineState> m_ToneMapPSO;
    rhi::DescriptorSetLayoutHandle m_ToneMapLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_ToneMapSet    = rhi::kInvalidSet;
    rhi::ShaderBytecode m_ToneMapVS;
    rhi::ShaderBytecode m_ToneMapFS;

    // 基础色纹理（默认 1×1 白色，运行时替换为 glTF 纹理）
    std::unique_ptr<rhi::IRHITexture>  m_DefaultBaseColorTex;
    std::unique_ptr<rhi::IRHISampler>  m_DefaultBaseColorSampler;

    // 天空盒（全屏三角形，无需 VB/IB）
    std::unique_ptr<rhi::IRHIPipelineState> m_SkyboxPSO;
    rhi::DescriptorSetLayoutHandle m_SkyboxDescLayout = rhi::kInvalidLayout;

    // Phase 5-4 多线程录制
    bool m_MultiThreadRecord = true;
    static constexpr u32 kMaxSecRecordLists = 8;
    std::vector<std::unique_ptr<rhi::IRHICommandList>> m_SecRecordLists;
    rhi::ShaderBytecode m_SkyboxVS;
    rhi::ShaderBytecode m_SkyboxFS;

    // 着色器字节码
    rhi::ShaderBytecode m_VS;
    rhi::ShaderBytecode m_FS;

    // 子系统
    std::unique_ptr<IGlobalIllumination> m_GI;          // GI 子系统（IBL）
    std::unique_ptr<IShadowSystem>       m_ShadowSystem; // 阴影子系统（可替换实现）

private:
    // 更新描述符集 IBL 绑定
    void UpdateIBLBindings(GI_IBL* gi);
};

} // namespace he::render
