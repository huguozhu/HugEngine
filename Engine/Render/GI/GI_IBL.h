#pragma once

#include "Threading/RenderThreadContext.h"   // T2.6：帧上下文（唯一 RHI 出口）

#include "RHI/RHI.h"
#include "RHI/Buffer.h"
#include "Math/Math.h"
#include "GI/GlobalIllumination.h"

namespace he::render {

// ============================================================================
// GI_IBL — 基于图像的光照（Image-Based Lighting）
//
// 从 Skybox Cubemap 生成三张贴图：
//   1. Irradiance Map（32×32 漫反射辐照度 Cubemap）
//   2. Prefilter Map（128×128 预滤波镜面反射 Cubemap，5 mip 级对应 roughness 0-1）
//   3. BRDF LUT（512×512 2D，split-sum BRDF 积分查找表）
//
// 使用光栅化路径（全屏三角形 + 逐面 offscreen pass），无需 Compute Shader。
// 仅在 Skybox Cubemap 变化时重新生成（脏标记机制）。
// ============================================================================
class GI_IBL : public IGlobalIllumination {

    // ── 【T2.6】帧上下文与统一解析入口（m_Device 继承自基类）──
    const RenderThreadContext* m_FrameCtx = nullptr;
    void SetFrameContext(const RenderThreadContext* ctx) { m_FrameCtx = ctx; }
    [[nodiscard]] rhi::IRHIBuffer* ResB(rhi::RHIBufferHandle h) const {
        const RenderThreadContext* c = m_FrameCtx ? m_FrameCtx : GetFrameContext();
        if (c) return c->ResolveBuffer(h);
        return m_Device ? m_Device->ResolveBuffer(h) : nullptr;
    }
    [[nodiscard]] rhi::IRHITexture* ResT(rhi::RHITextureHandle h) const {
        const RenderThreadContext* c = m_FrameCtx ? m_FrameCtx : GetFrameContext();
        if (c) return c->ResolveTexture(h);
        return m_Device ? m_Device->ResolveTexture(h) : nullptr;
    }
    [[nodiscard]] rhi::IRHIPipelineState* ResP(rhi::RHIPipelineStateHandle h) const {
        const RenderThreadContext* c = m_FrameCtx ? m_FrameCtx : GetFrameContext();
        if (c) return c->ResolvePipelineState(h);
        return m_Device ? m_Device->ResolvePipelineState(h) : nullptr;
    }
    [[nodiscard]] rhi::IRHISampler* ResS(rhi::RHISamplerHandle h) const {
        const RenderThreadContext* c = m_FrameCtx ? m_FrameCtx : GetFrameContext();
        if (c) return c->ResolveSampler(h);
        return m_Device ? m_Device->ResolveSampler(h) : nullptr;
    }

public:
    GI_IBL()  = default;
    ~GI_IBL() override = default;

    // ---- IRenderSubsystem 接口 ----
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) override;
    void Shutdown() override;
    void Update(const SubsystemContext& ctx) override;
    void Render(rhi::IRHICommandList* cmdList) override;
    void Bind(rhi::IRHICommandList* cmdList) const override;
    void OnResize(u32 width, u32 height) override;

    [[nodiscard]] const char* GetName()  const override { return "GI_IBL"; }
    [[nodiscard]] bool        IsReady()  const override { return m_Ready; }
    [[nodiscard]] bool        IsEnabled() const override { return m_Enabled; }
    void SetEnabled(bool enabled) override { m_Enabled = enabled; }

    // ---- IGlobalIllumination 接口 ----
    [[nodiscard]] GIMode GetMode() const override { return GIMode::IBL; }
    [[nodiscard]] rhi::IRHITexture* GetIndirectDiffuseTexture()  const override { return m_Device->ResolveTexture(m_IrradianceMap); }
    [[nodiscard]] rhi::IRHITexture* GetIndirectSpecularTexture() const override { return m_Device->ResolveTexture(m_PrefilterMap); }

    // ---- 设置 Skybox Cubemap ----
    /// 设置输入天空盒 Cubemap + 采样器，标记脏状态触发重生成
    void SetIBLSkybox(rhi::IRHITexture* cubemap, rhi::IRHISampler* sampler);
    bool IsDirty() const { return m_Dirty; }

    // ---- 访问 IBL 纹理（供 ForwardPipeline 绑定到 PBR 描述符集） ----
    [[nodiscard]] rhi::IRHITexture* GetIrradianceMap() const { return m_Device->ResolveTexture(m_IrradianceMap); }
    [[nodiscard]] rhi::IRHITexture* GetPrefilterMap()  const { return m_Device->ResolveTexture(m_PrefilterMap); }
    [[nodiscard]] rhi::IRHITexture* GetBRDF_LUT()      const { return m_Device->ResolveTexture(m_BRDF_LUT); }
    [[nodiscard]] rhi::IRHISampler* GetIBLSampler()    const { return m_Device->ResolveSampler(m_IBLSampler); }

private:
    // 渲染 6 个 Cubemap 面 + 指定 mip level
    void RenderCubemapFaces(rhi::IRHICommandList* cmd,
                            rhi::IRHIPipelineState* pso,
                            rhi::IRHITexture* target,
                            u32 mipLevel, u32 resolution,
                            const float4x4& proj);

    // 获取 Cubemap 面 i 的 View 矩阵
    static float4x4 CubeFaceViewMatrix(u32 face);

    // GPU 资源
    rhi::RHITextureHandle m_IrradianceMap{};   // A-3：32×32 RGBA16_FLOAT Cubemap
    rhi::RHITextureHandle m_PrefilterMap{};     // A-3：128×128 RGBA16_FLOAT Cubemap (5 mips)
    rhi::RHITextureHandle m_BRDF_LUT{};         // A-3：512×512 RG16_FLOAT 2D
    std::vector<void*> m_CachedMipViews;                  // 逐 mip 面视图缓存（RHI 不透明句柄，延迟销毁用）
    rhi::RHISamplerHandle m_IBLSampler{};       // A-3：线性 Clamp 采样器（共享）

    // PSO
    rhi::RHIPipelineStateHandle m_IrradiancePSO{};   // A-3
    rhi::RHIPipelineStateHandle m_PrefilterPSO{};   // A-3
    rhi::RHIPipelineStateHandle m_BRDF_LUT_PSO{};   // A-3

    // Descriptor Sets
    rhi::DescriptorSetLayoutHandle m_IrradianceLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_IrradianceSet    = rhi::kInvalidSet;
    rhi::DescriptorSetLayoutHandle m_PrefilterLayout  = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_PrefilterSet     = rhi::kInvalidSet;

    // 输入 Skybox Cubemap（外部提供，不持有所有权）
    rhi::IRHITexture* m_SkyboxCubemap  = nullptr;
    rhi::IRHISampler* m_SkyboxSampler  = nullptr;
    bool              m_Dirty          = true;  // Skybox 改变后需重生成
    bool              m_Ready          = false;

    // 分辨率参数
    static constexpr u32 kIrradianceRes = 32;
    static constexpr u32 kPrefilterRes  = 128;
    static constexpr u32 kPrefilterMips = 5;
    static constexpr u32 kBRDF_LUT_Res  = 512;
};

} // namespace he::render
