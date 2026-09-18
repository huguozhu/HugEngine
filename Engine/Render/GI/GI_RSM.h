#pragma once

#include "GI/GlobalIllumination.h"
#include "RHI/RHI.h"
#include "RHI/Buffer.h"
#include "Math/Math.h"

namespace he::render {

// RSM（Reflective Shadow Map）默认分辨率
constexpr u32 kDefaultRSMResolution = 512;

// ============================================================================
// GI_RSM — Reflective Shadow Maps 全局光照
//
// 在方向光 Shadow Pass 后用相同视角渲染 RSM 数据（位置/法线/通量），
// PBR Shader 中在 light space 采样 RSM 获取单次反弹间接漫反射。
//
// 纹理：
//   - RSM_Position: RGBA16_FLOAT（worldPos.xyz）
//   - RSM_Flux:     RGBA16_FLOAT（worldNormal.xy + flux）
// ============================================================================
class GI_RSM : public IGlobalIllumination {
public:
    GI_RSM()  = default;
    ~GI_RSM() override = default;

    // IRenderSubsystem
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) override;
    void Shutdown() override;
    void Update(const SubsystemContext& ctx) override;
    void Render(rhi::IRHICommandList* cmdList) override;
    void Bind(rhi::IRHICommandList* cmdList) const override;
    void OnResize(u32 width, u32 height) override;

    const char* GetName()  const override { return "GI_RSM"; }
    bool        IsReady()  const override { return m_Ready; }
    bool        IsEnabled() const override { return m_Enabled; }
    void SetEnabled(bool e) override { m_Enabled = e; }

    // IGlobalIllumination
    GIMode GetMode() const override { return GIMode::RSM; }

    // ---- 设置 RSM 参数 ----
    void SetLightViewProj(const float4x4& vp, u32 resolution,
                          rhi::IRHIBuffer* objBuf, rhi::IRHISampler* shadowSampler,
                          rhi::DescriptorSetHandle descSet);

    // 设置 Shadow Map 深度附件（渲染 RSM 时复用）
    // DEPRECATED: RSM 现在使用独立深度缓冲，不再复用 CSM ShadowMap
    void SetShadowDepthView(void* depthView) { m_ExternalDepthView = depthView; }

    /// 设置本帧的 GPULight SSBO（RSM_Generate.frag 的 u_Lights@binding 1）。
    /// 【为什么必须单独给】`RenderRSMPass` 此前把**对象缓冲**同时绑到了 binding 1 与 2，
    /// 于是着色器里的 `u_Lights[0]` 实际读到的是 `GPUObjectData[0]`（世界矩阵被当成光源颜色
    /// 与强度解释）⇒ 通量是人造值/garbage，RSM 间接光因此常年恒为 0（实测 S_rsm = 0 到 1e-7，
    /// 而 Lighting 里那段 VPL 求和的成本照样在付）。见文档 §9.2-AA。
    void SetLightBuffer(rhi::IRHIBuffer* lightBuffer) { m_ExternalLightBuf = lightBuffer; }

    // 从光源 POV 渲染几何体到 RSM 纹理（使用独立深度缓冲）
    void RenderRSMPass(rhi::IRHICommandList* cmd, he::World& world, he::SceneGraph& sg);

    // 纹理访问
    rhi::IRHITexture* GetRSMPositionMap() const { return m_RSMPos.get(); }
    rhi::IRHITexture* GetRSMFluxMap()     const { return m_RSMFlux.get(); }
    rhi::IRHISampler* GetRSMSampler()     const { return m_RSMSampler.get(); }

private:
    std::unique_ptr<rhi::IRHITexture> m_RSMPos;   // RGBA16_FLOAT worldPos
    std::unique_ptr<rhi::IRHITexture> m_RSMFlux;   // RGBA16_FLOAT normal+flux
    std::unique_ptr<rhi::IRHITexture> m_RSMDepth;  // D32_FLOAT 独立深度缓冲（不依赖 CSM ShadowMap）
    std::unique_ptr<rhi::IRHISampler> m_RSMSampler;

    std::unique_ptr<rhi::IRHIPipelineState> m_RSMPSO;
    rhi::DescriptorSetLayoutHandle m_RSMLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_RSMSet    = rhi::kInvalidSet;
    void* m_ExternalDepthView = nullptr;  // 外部深度视图（保留兼容但不再用于 RSM）

    float4x4 m_LightVP;
    u32      m_RSMResolution = kDefaultRSMResolution;
    rhi::IRHIBuffer*        m_ExternalObjBuf = nullptr;
    rhi::IRHIBuffer*        m_ExternalLightBuf = nullptr;   // GPULight SSBO（见 SetLightBuffer）
    rhi::DescriptorSetHandle m_ExternalDescSet = rhi::kInvalidSet;

    bool m_Ready = false;
};

} // namespace he::render
