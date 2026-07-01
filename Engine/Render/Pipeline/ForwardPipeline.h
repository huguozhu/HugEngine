#pragma once

#include "Pipeline/Material.h"
#include "Pipeline/Camera.h"
#include "GI/GlobalIllumination.h"
#include "RHI/RHI.h"

namespace he::render { class GI_IBL; }  // 前向声明 — 避免循环依赖
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

    // 推进三缓冲槽位，更新 per-mesh 描述符集指向新帧缓冲区（帧首调用，在 Shadow/Scene 之前）
    void NextFrame();
    void BeginFrame(rhi::IRHICommandList* cmd, u32 width, u32 height);

    // 渲染场景（含阴影通道）
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

public:
    // --- 阴影通道编排（在主管线渲染通道外调用）---
    // 收集阴影投射光源并上传阴影数据到 SSBO
    void CollectShadowLights(
        he::World& world, he::SceneGraph& sg,
        std::vector<const he::LightComponent*>& shadowLights,
        std::vector<GPUShadowData>& shadowGPUData,
        const CameraData& camera);
    // CSM 级联阴影通道（cascadeIdx ∈ [0, CASCADE_COUNT)）
    void BeginShadowPass(rhi::IRHICommandList* cmd, u32 cascadeIdx);
    void RenderShadowPass(
        rhi::IRHICommandList* cmd,
        he::World& world,
        he::SceneGraph& sg,
        const std::vector<const he::LightComponent*>& shadowLights,
        const std::vector<GPUShadowData>& shadowGPUData,
        u32 cascadeIdx);
    void EndShadowPass(rhi::IRHICommandList* cmd);
    // CSM: 全部级联渲染完毕后统一转换布局
    void EndAllShadowPasses(rhi::IRHICommandList* cmd);

    // --- 点光源阴影（Cubemap）---
    void RenderPointShadowPass(
        rhi::IRHICommandList* cmd,
        he::World& world,
        he::SceneGraph& sg,
        const std::vector<const he::LightComponent*>& pointLights,
        const std::vector<GPUShadowData>& pointShadowData);
    bool HasPointShadows() const { return m_HasPointShadows; }

    // Phase 5-4: 多线程命令录制开关（ImGui 面板运行时切换）
    void SetMultiThreadedRecording(bool enable) { m_MultiThreadRecord = enable; }
    bool IsMultiThreadedRecording() const { return m_MultiThreadRecord; }

    // --- HDR 离屏渲染 ---
    void BeginHDRPass(rhi::IRHICommandList* cmd, u32 width, u32 height);
    void EndHDRPass(rhi::IRHICommandList* cmd);
    void RenderToneMapPass(rhi::IRHICommandList* cmd);
    void ResizeHDRTarget(u32 width, u32 height);

private:

    // 从 World 收集活跃光源，填充 PushConstant 的光照字段
    void CollectLights(PushConstantData& pc,
                       std::vector<GPUShadowData>& shadowData,
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
    std::unique_ptr<rhi::IRHIPipelineState> m_ShadowPSO;  // 阴影深度专用 PSO

    // 主管道 Descriptor Set + Storage Buffers（光照 + 对象数据 + 阴影数据 + 阴影贴图）
    rhi::DescriptorSetLayoutHandle m_DescLayout      = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_DescSets[MAX_FRAMES_IN_FLIGHT] = {};  // 三缓冲共享描述符集
    // 三缓冲 StorageBuffer（Phase 1 多线程渲染：CPU 写入当前帧，GPU 读取上一帧）
    std::unique_ptr<rhi::IRHIBuffer> m_LightBuffers[MAX_FRAMES_IN_FLIGHT];
    std::unique_ptr<rhi::IRHIBuffer> m_ObjectBuffers[MAX_FRAMES_IN_FLIGHT];  // GPUObjectData[MAX_OBJECTS]
    std::unique_ptr<rhi::IRHIBuffer> m_ShadowBuffers[MAX_FRAMES_IN_FLIGHT];  // GPUShadowData[MAX_SHADOWS]
    u32 m_CurrentFrameSlot = 0;  // 当前帧槽位 [0, MAX_FRAMES_IN_FLIGHT)
    // 所有已分配的 per-mesh 描述符集（用于每帧更新动态绑定 1-3）
    std::vector<rhi::DescriptorSetHandle> m_AllPerMeshDescSets;

    // CSM 阴影贴图（3 级联，每级 2048×2048 D32_FLOAT）
    std::unique_ptr<rhi::IRHITexture>  m_ShadowMaps[CASCADE_COUNT];
    std::unique_ptr<rhi::IRHISampler>  m_ShadowSampler;      // 方向光阴影采样器（共享）
    std::unique_ptr<rhi::IRHISampler>  m_ShadowPlaceholderSampler; // 占位纹理的普通采样器
    u32 m_ShadowMapSize = 2048;
    std::unique_ptr<rhi::IRHITexture>  m_ShadowPlaceholderTex; // 占位纹理

    // 点光源阴影（Cubemap）
    std::unique_ptr<rhi::IRHITexture>  m_PointShadowMap;     // Cubemap 深度纹理
    std::unique_ptr<rhi::IRHISampler>  m_PointShadowSampler; // Cubemap 采样器
    u32 m_PointShadowMapSize = 512;  // 每面分辨率
    bool m_HasPointShadows = false;
    bool m_PointShadowInitDone = false;  // Cubemap 布局首次转换标记

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

    // Phase 5-4 多线程录制（暂时禁用，SecCB viewport 集成需 Vulkan 深度调试）
    // TODO: 修复后启用 — SecCB 从 primary 继承 viewport 策略需重构
    bool m_MultiThreadRecord = false;
    static constexpr u32 kMaxSecRecordLists = 8;
    std::vector<std::unique_ptr<rhi::IRHICommandList>> m_SecRecordLists;
    rhi::ShaderBytecode m_SkyboxVS;
    rhi::ShaderBytecode m_SkyboxFS;

    // 着色器字节码
    rhi::ShaderBytecode m_VS;
    rhi::ShaderBytecode m_FS;
    rhi::ShaderBytecode m_ShadowVS;
    rhi::ShaderBytecode m_ShadowFS;

    // GI 子系统
    std::unique_ptr<IGlobalIllumination> m_GI;

public:
    // 渲染天空盒（遍历 Scene 中的 SkyboxComponent）
    void RenderSkybox(rhi::IRHICommandList* cmd, he::World& world,
                      const CameraData& camera);

    // GI 子系统访问
    IGlobalIllumination* GetGI() { return m_GI.get(); }
    void SetGI(std::unique_ptr<IGlobalIllumination> gi) { m_GI = std::move(gi); }

    // GI 准备（Scene 渲染前调用：检测 Skybox → 更新 IBL 贴图）
    void PrepareGI(rhi::IRHICommandList* cmd, he::World& world);

private:
    // 更新描述符集 IBL 绑定
    void UpdateIBLBindings(GI_IBL* gi);
};

} // namespace he::render
