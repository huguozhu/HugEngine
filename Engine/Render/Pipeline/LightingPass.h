#pragma once

#include "RHI/RHI.h"
#include "Pipeline/Material.h"
#include "Pipeline/ClusteredShading.h"
#include "Shadow/IShadowSystem.h"
#include <memory>
#include <vector>

namespace he::render {

// 前向声明
class GBufferRenderer;
struct RenderGraph;

// ============================================================
// 光源输入模式枚举（LightingPass 支持多种输入源组合）
// ============================================================
enum class LightingSource : u8 {
    None = 0,
    // 阴影
    Shadow_CSM,     // 传统 CSM + Spot Shadow Maps
    Shadow_RT,      // 硬件 Ray Tracing 阴影
    // 环境光遮蔽
    AO_SSAO,        // 屏幕空间 AO
    AO_RTAO,        // 硬件 Ray Tracing AO
    // 镜面反射
    Specular_SSR,   // 屏幕空间反射
    Specular_RT,    // 硬件 Ray Tracing 反射
    // 间接漫反射
    Diffuse_SSGI,   // 屏幕空间 GI
    Diffuse_RTGI,   // 硬件 Ray Tracing GI
    Diffuse_DDGI,   // DDGI 探针 GI
};

// ============================================================
// 光照输入源配置
// ============================================================
struct LightingInputSources {
    LightingSource shadow   = LightingSource::Shadow_CSM;
    LightingSource ao       = LightingSource::AO_SSAO;
    LightingSource specular = LightingSource::Specular_SSR;
    LightingSource diffuse  = LightingSource::Diffuse_SSGI;
    bool useDDGI = true;  // DDGI 可与任意 diffuse 模式叠加
};

// ============================================================
// LightingPass — 延迟光照 Pass（共享组件）
//
// 拥有 HDR 目标纹理 + Lighting PSO + 描述符集
// 提供统一 Render 接口：输入 GBuffer + 效果纹理 → 输出 HDR
//
// 供 DeferredPipeline 使用。
// ============================================================
class LightingPass {
    HE_DECLARE_NON_COPYABLE(LightingPass);

public:
    LightingPass()  = default;
    ~LightingPass() = default;

    // ── 生命周期 ──
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();
    void OnResize(rhi::IRHIDevice* device, u32 width, u32 height);

    // ── 渲染（每帧调用，由 BuildFrameGraph 的 Lighting pass lambda 调用）──
    // 执行完整的延迟光照：描述符集绑定 → 聚集着色（可选）→ 全屏三角形绘制
    void Render(rhi::IRHICommandList* cmd,
                // GBuffer 纹理（来自 GBufferRenderer）
                rhi::IRHITexture* gbA, rhi::IRHITexture* gbB, rhi::IRHITexture* gbC,
                rhi::IRHITexture* gbDepth, rhi::IRHITexture* gbE,
                rhi::IRHITexture* gbDisneyA, rhi::IRHITexture* gbDisneyB,
                // 阴影贴图（来自 ShadowSystem）
                rhi::IRHITexture* csmShadow0, rhi::IRHITexture* csmShadow1,
                rhi::IRHITexture* csmShadow2, rhi::IRHITexture* spotShadow,
                // 光源/阴影数据 SSBO
                rhi::IRHIBuffer* lightBuffer, rhi::IRHIBuffer* shadowBuffer,
                // 屏幕空间效果
                rhi::IRHITexture* ssaoTex,
                rhi::IRHITexture* ssgiTex, rhi::IRHISampler* ssgiSampler,
                rhi::IRHITexture* ssrTex,  rhi::IRHISampler* ssrSampler,
                rhi::IRHIBuffer*  ddgiProbeBuffer,
                // 聚集着色（可选，nullptr 时跳过）
                ClusteredShading* clusteredShading,
                rhi::IRHIBuffer* lightGridBuffer,
                rhi::IRHIBuffer* lightIndexListBuffer,
                std::vector<GPULight>* cachedLights,
                // RT 效果纹理（可选，暂未使用，保留供未来 RT 管线扩展）
                rhi::IRHITexture* rtShadowMask  = nullptr,
                rhi::IRHITexture* rtReflection  = nullptr,
                rhi::IRHITexture* rtAO          = nullptr,
                rhi::IRHITexture* rtGI          = nullptr,
                // 相机参数
                const float4& cameraPos = float4(0,0,0,1),
                float iblIntensity = 1.0f,
                u32 lightCount = 0,
                u32 width = 0, u32 height = 0);

    // ── 访问器 ──
    rhi::IRHITexture*   GetHDRTarget()  const { return m_HDRTarget.get(); }
    rhi::IRHITexture*   GetHDRDepth()   const { return m_HDRDepth.get(); }
    rhi::IRHISampler*   GetHDRSampler() const { return m_HDRSampler.get(); }
    rhi::IRHISampler*   GetPointSampler() const { return m_PointSampler.get(); }
    rhi::IRHIPipelineState* GetPSO()     const { return m_PSO.get(); }
    rhi::DescriptorSetHandle GetDescriptorSet() const { return m_Set; }

    // 设置 IBL 贴图（Irradiance/Prefilter/BRDF LUT），供天空盒喂 IBL 间接光
    void SetIBLTextures(rhi::IRHITexture* irradiance, rhi::IRHITexture* prefilter,
                        rhi::IRHITexture* brdfLut, rhi::IRHISampler* sampler);

    // 设置空中透视参数（太阳方向 + 浑浊度，来自 PhysicalSkyComponent）
    void SetAtmosphere(float3 sunDir, float turbidity);

private:
    void CreateHDRTextures(rhi::IRHIDevice* device);
    void CreatePSOAndDescriptorSet(rhi::IRHIDevice* device);

    // ── HDR 目标纹理 ──
    std::unique_ptr<rhi::IRHITexture> m_HDRTarget, m_HDRDepth;
    std::unique_ptr<rhi::IRHISampler> m_HDRSampler, m_PointSampler;

    rhi::IRHIDevice* m_Device = nullptr;

    // ── Lighting PSO + 描述符集 ──
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;

    u32 m_Width = 0, m_Height = 0;
    bool m_MSAAEnabled = false;  // 供外部 MSAA 覆盖（Init 前设置）
    float3 m_AtmSunDir    = float3(0, 1, 0);  // 空中透视太阳方向（默认朝天）
    float  m_AtmTurbidity = 0.0f;             // 空中透视浑浊度（0=关闭哨兵；物理范围 1~10）
};

} // namespace he::render
