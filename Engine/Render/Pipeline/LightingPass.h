#pragma once

#include "RHI/RHI.h"
#include "Pipeline/Material.h"
#include "Pipeline/ClusteredShading.h"
#include "Shadow/IShadowSystem.h"
#include "GI/GITypes.h"   // ShadowChannel / GIBlendMode / 源槽与通道混合参数（RHI-free）
#include <memory>
#include <vector>

namespace he::render {

// 前向声明
class GBufferRenderer;
struct RenderGraph;

// ============================================================
// 注（P0/D2 数据模型下沉）：
// ShadowChannel / GIBlendMode / GISourceSlotData / GIChannelBlendData
// 已迁至 GI/GITypes.h —— 那里只依赖 Core/Types.h，不含任何 RHI 类型，
// 使单元测试可直接包含并验证层栈与降级逻辑，无需链接 HugEngineRender。
// ============================================================

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
    /// RSM 间接光**辐照度 E**（半分辨率，任务 16）：由 GI/RSM_Indirect.frag.slang 产出。
    /// 为空 = 本帧没有产出（RSM 不在层栈 / 没有活动阴影）⇒ 必须显式回绑黑色占位，
    /// 否则会接着采样上一帧的绑定（§9.2-T）。采样用线性 clamp 以便升采样。
    rhi::IRHITexture* rsmIndirectTex = nullptr;
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
    /// 本帧使用的飞行帧槽位（0..MAX_FRAMES_IN_FLIGHT-1）。
    /// Lighting 有一批**逐帧轮换的资源**（光源/阴影/探针 SSBO、合成参数 UBO…）与**一份**
    /// 描述符集：若每帧都往同一份集合里重绑当前槽位的缓冲，GPU 执行上一帧时可能已经读到
    /// 本帧刚写进去的绑定 —— 这正是 §9.2-J 描述的那类隐患（此前靠"值变化小"掩盖）。
    /// 传入槽位后，本 pass 会绑定该槽位**自己的**描述符集，重绑不再跨帧。
    u32    frameSlot = 0;
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
    /// 取某飞行帧槽位的描述符集（默认第 0 份，供不需要区分的调用方/调试用）
    rhi::DescriptorSetHandle GetDescriptorSet(u32 slot = 0) const {
        return m_Sets[slot % rhi::kMaxFramesInFlight];
    }

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

    // ── Lighting PSO + 描述符集（每飞行帧一份）──
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    /// 合成参数 UBO：**每飞行帧一份**（§9.2-J）。单份时本帧写入会覆盖仍在飞行的上一帧
    /// 所读的参数——此前靠"值变化小"掩盖。
    std::unique_ptr<rhi::IRHIBuffer> m_BlendUBO[rhi::kMaxFramesInFlight];
    /// 每飞行帧一份描述符集：逐帧轮换的资源（光源/阴影/探针 SSBO 与上面的 UBO）各自绑进
    /// 自己槽位的集合，重绑不再跨帧。渲染时按 `LightingInputs::frameSlot` 取。
    rhi::DescriptorSetHandle       m_Sets[rhi::kMaxFramesInFlight] = {};

    // ── 中性占位纹理（1×1）──
    // 输入为 null 时**必须显式回绑**其中一张，不能只是"跳过更新"：描述符集是持久的，
    // 跳过会留下上一帧的绑定，于是本帧没有产出的纹理仍会被采样到（§9.2-T）。
    //   White：语义 1.0 —— 无遮挡/无遮蔽（阴影图、AO）
    //   Black：语义 0.0 —— 无贡献（间接光、反射、RSM 位置与通量）
    std::unique_ptr<rhi::IRHITexture> m_PlaceholderWhite;
    std::unique_ptr<rhi::IRHITexture> m_PlaceholderBlack;
    std::unique_ptr<rhi::IRHITexture> m_PlaceholderCube;   // 黑色 Cubemap（IBL 无贡献）
    std::unique_ptr<rhi::IRHISampler> m_PlaceholderSampler;

    u32 m_Width = 0, m_Height = 0;
    bool m_MSAAEnabled = false;  // 供外部 MSAA 覆盖（Init 前设置）
    float3 m_AtmSunDir    = float3(0, 1, 0);  // 空中透视太阳方向（默认朝天）
    float  m_AtmTurbidity = 0.0f;             // 空中透视浑浊度（0=关闭哨兵；物理范围 1~10）
};

} // namespace he::render
