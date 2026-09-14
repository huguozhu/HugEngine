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
// 阴影通道枚举
//
// 注（架构演进后）：diffuse / specular / ao 三个「能量通道」已由 GIConfig 的
// 层栈（GIChannelStack + GISourceId）表达，原先的 AOChannel / SpecularChannel /
// DiffuseChannel 三个单值枚举与 GIChannels 结构已无使用者，已移除。
// 阴影是「可见性（乘法项）」而非能量（加法项），不适用层栈语义，故保留本枚举。
// ============================================================
enum class ShadowChannel : u8 { None = 0, Raster, RT };  // 阴影：光栅化（CSM/点光 cubemap/聚光 map）/ 硬件光追

// ============================================================
// GI 分层合成（通道通用——diffuse / specular / AO 同构）
//
// 每个通道都可能同时有多个 GI 源（屏幕空间 / 光追 / 低频探针或环境），
// 它们描述的是同一个物理量 → 直接相加会双重计数，必须归一化合成。
//
//   · 权重（Weight）        —— 各源相对权重（配合各源内部置信度使用）
//   · 衰减距离（FalloffDistance）—— 可选的「距离让位」（0=不启用）
//
// 注意：物理正确性来自「权重归一化」，距离衰减只是性能/艺术控制的让位机制；
// 各源可信度主要由置信度决定（屏幕空间看可见性、光追看收敛度、探针看可见性），
// 与「距离」无必然关系。
// ============================================================

/// 多源间接光的合成方式
enum class GIBlendMode : u8 {
    Additive   = 0,   // 直接相加（旧行为——双重计数，仅作 A/B 对照）
    Normalized = 1,   // 归一化加权：Σ(源×w)/Σw，权重和=1 → 无双重计数（推荐）
};

/// 单个 GI 源槽（与 shader 的 GISourceSlot 布局一致）
struct GISourceSlotData {
    u32   id              = 0;        // GISourceId（决定 shader 走哪条采样分支）
    float weight          = 0.0f;     // 相对权重（0 = 不参与）
    float falloffDistance = 0.0f;     // 「距离让位」（0 = 不启用）
    u32   _pad            = 0;
};

/// 单通道的合成参数（与 shader 的 GIChannelBlendParams 布局一致）
///
/// Wave 1：由「三个固定语义槽」改为「源数组」——槽位是通用容器，语义由 id 决定。
/// 这样新增 GI 只需 GISourceId 加一项 + SampleSource 加一个 case，
/// 不再需要改 UBO 结构 / 帧图映射 / shader 分支。
struct GIChannelBlendData {
    static constexpr u32 kMaxSources = 4;
    GISourceSlotData sources[kMaxSources];
    u32 count       = 0;
    u32 mode        = 1;   // GIBlendMode（0=相加对照, 1=归一化加权）
    u32 furnaceMode = 0;   // 白炉数值测试（Wave 0.2）
    u32 _pad        = 0;

    /// 追加一个源（weight<=0 忽略；超出容量忽略）
    void Add(u32 sourceId, float w, float falloff = 0.0f) {
        if (w <= 0.0f || count >= kMaxSources) return;
        sources[count].id              = sourceId;
        sources[count].weight          = w;
        sources[count].falloffDistance = falloff;
        count++;
    }
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
    // ── 分层合成（P2：多源间接光的归一化加权，通道通用）──
    // 混合参数经 UBO 传递给 shader（3 通道 × 24B，避免超出 push constant 128B 上限）
    // ── 分层合成（Wave 1：源数组，按源 id 分派采样）──
    GIChannelBlendData diffuseBlend;    // 间接漫反射（IBL/DDGI/SSGI/RSM/RTGI 任意组合）
    GIChannelBlendData specularBlend;   // 间接镜面（IBL/SSR/RT 反射）
    GIChannelBlendData aoBlend;         // 环境光遮蔽（SSAO/RTAO）
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
    std::unique_ptr<rhi::IRHIBuffer> m_BlendUBO;   // GI 分层合成参数 UBO（3 通道混合参数）
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;

    u32 m_Width = 0, m_Height = 0;
    bool m_MSAAEnabled = false;  // 供外部 MSAA 覆盖（Init 前设置）
    float3 m_AtmSunDir    = float3(0, 1, 0);  // 空中透视太阳方向（默认朝天）
    float  m_AtmTurbidity = 0.0f;             // 空中透视浑浊度（0=关闭哨兵；物理范围 1~10）
};

} // namespace he::render
