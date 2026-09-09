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
// 4 个通道的独立枚举（类型安全：每个通道只能选本通道技术）
// ============================================================
enum class ShadowChannel : u8 { None = 0, Raster, RT };  // 阴影：光栅化（CSM/点光 cubemap/聚光 map）/ 硬件光追
enum class AOChannel : u8 { None = 0, SSAO, RTAO };           // 环境光遮蔽：SSAO / RT AO
enum class SpecularChannel : u8 { None = 0, SSR, RT };        // 镜面反射：SSR / RT 反射
enum class DiffuseChannel : u8 { None = 0, SSGI, RTGI };// 间接漫反射：SSGI / RT GI（DDGI 由 ddgiOverlay 独立叠加）

// ============================================================
// 光照通道配置（4 通道技术选型）
// ============================================================
struct GIChannels {
    ShadowChannel   shadow   = ShadowChannel::Raster;
    AOChannel       ao       = AOChannel::SSAO;
    SpecularChannel specular = SpecularChannel::SSR;
    DiffuseChannel  diffuse  = DiffuseChannel::SSGI;
    bool ddgiOverlay = true;  // DDGI 可与任意 diffuse 模式叠加
};

// ============================================================
// LightingInputs — 渲染输入（打包 Render 的全部输入）
//
// M1.1：把 LightingPass::Render 的 33 参数收敛为结构体。
// 仅承载输入数据，不持有资源；生命周期由调用方管理。
// ============================================================
struct LightingInputs {
    // GBuffer 纹理
    rhi::IRHITexture* gbA = nullptr;
    rhi::IRHITexture* gbB = nullptr;
    rhi::IRHITexture* gbC = nullptr;
    rhi::IRHITexture* gbDepth = nullptr;
    rhi::IRHITexture* gbE = nullptr;
    rhi::IRHITexture* gbDisneyA = nullptr;
    rhi::IRHITexture* gbDisneyB = nullptr;
    // 阴影贴图
    rhi::IRHITexture* csmShadow0 = nullptr;
    rhi::IRHITexture* csmShadow1 = nullptr;
    rhi::IRHITexture* csmShadow2 = nullptr;
    rhi::IRHITexture* spotShadow = nullptr;
    // 光源/阴影数据 SSBO
    rhi::IRHIBuffer* lightBuffer = nullptr;
    rhi::IRHIBuffer* shadowBuffer = nullptr;
    // 屏幕空间效果
    rhi::IRHITexture* ssaoTex = nullptr;
    rhi::IRHITexture* ssgiTex = nullptr;
    rhi::IRHISampler* ssgiSampler = nullptr;
    rhi::IRHITexture* ssrTex = nullptr;
    rhi::IRHISampler* ssrSampler = nullptr;
    // DDGI 探针
    rhi::IRHIBuffer* ddgiProbeBuffer = nullptr;
    rhi::IRHIBuffer* ddgiGridUniform = nullptr;
    // RSM 间接光（可选，非空时 shader 采样 RSM 间接漫反射——Forward/Deferred 共用）
    rhi::IRHITexture* rsmPositionMap = nullptr;
    rhi::IRHITexture* rsmFluxMap     = nullptr;
    // RT 效果输出（可选，非空才替换占位）
    rhi::IRHITexture* rtShadowMask = nullptr;
    rhi::IRHITexture* rtReflection = nullptr;
    rhi::IRHITexture* rtAO = nullptr;
    rhi::IRHITexture* rtGI = nullptr;
    // 聚集着色（可选）
    ClusteredShading* clusteredShading = nullptr;
    rhi::IRHIBuffer* lightGridBuffer = nullptr;
    rhi::IRHIBuffer* lightIndexListBuffer = nullptr;
    std::vector<GPULight>* cachedLights = nullptr;
    // 相机/渲染参数
    float4 cameraPos = float4(0, 0, 0, 1);
    float  iblIntensity = 1.0f;
    u32    lightCount = 0;
    u32    width = 0, height = 0;
    // GI 通道参数（M1：强度由 push constant 驱动，替代 shader 魔法系数）
    float giIntensity = 1.0f;    // 间接漫反射 GI 总强度（ambient 系数）
    float aoIntensity = 1.0f;    // AO 强度
    float ddgiScale   = 1.0f;    // DDGI 贡献缩放
    bool  ddgiOverlay = true;    // DDGI 探针 GI 是否采样
    GIChannels sources; // 通道选择（shadow/ao/specular/diffuse）
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
    // M1.1：33 参数收敛为 LightingInputs 结构体
    void Render(rhi::IRHICommandList* cmd, const LightingInputs& in);

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
