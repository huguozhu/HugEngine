#pragma once

#include "PostProcess/BloomPass.h"
#include "PostProcess/DOFPass.h"
#include "PostProcess/MotionBlurPass.h"
#include "PostProcess/AutoExposurePass.h"
#include "PostProcess/ColorGradingPass.h"
#include "PostProcess/CameraEffectsPass.h"
#include "PostProcess/ToneMapPass.h"
#include "PostProcess/SkyboxPass.h"
#include "AntiAliasing/AntiAliasing.h"
#include "AntiAliasing/AA_FXAA.h"
#include "AntiAliasing/AA_SMAA.h"
#include "AntiAliasing/AA_MSAA.h"
#include "RHI/RHI.h"
#include <memory>

namespace he::render {

// ============================================================
// PostProcessChain — 后处理责任链（共享组件）
//
// 拥有所有后处理 Pass 实例，统一管理生命周期。
// RenderGraph 链逻辑保留在 BuildFrameGraph 中（避免破坏
// 复杂的输入源选择逻辑），通过 getter 访问各个 Pass。
//
// 供 DeferredPipeline 使用。
// ============================================================
class PostProcessChain {
    HE_DECLARE_NON_COPYABLE(PostProcessChain);

public:
    PostProcessChain()  = default;
    ~PostProcessChain() = default;

    // ── 生命周期 ──
    void Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();
    void OnResize(rhi::IRHIDevice* device, u32 width, u32 height);

    // ── Pass 访问器（供 BuildFrameGraph 编排）──
    BloomPass&        GetBloom()        { return m_Bloom; }
    DOFPass&          GetDOF()          { return m_DOF; }
    MotionBlurPass&   GetMotionBlur()   { return m_MotionBlur; }
    AutoExposurePass& GetAutoExposure() { return m_AutoExposure; }
    ColorGradingPass& GetColorGrading() { return m_ColorGrading; }
    CameraEffectsPass& GetCameraEffects() { return m_CameraEffects; }
    ToneMapPass*      GetToneMap()      { return m_ToneMap.get(); }
    SkyboxPass*       GetSkybox()       { return m_Skybox.get(); }
    IAntiAliasing*    GetTAA()          { return m_TAA.get(); }

    // ── AA 开关 ──
    void EnableFXAA(rhi::IRHIDevice* device, u32 w, u32 h, bool enable);
    bool IsFXAAEnabled() const { return m_FXAAEnabled && m_FXAA != nullptr; }
    AA_FXAA* GetFXAA()         { return m_FXAA.get(); }

    void EnableSMAA(rhi::IRHIDevice* device, u32 w, u32 h, bool enable);
    bool IsSMAAEnabled() const { return m_SMAAEnabled && m_SMAA != nullptr && m_SMAA->IsReady(); }
    AA_SMAA* GetSMAA()         { return m_SMAA.get(); }

    void EnableMSAA(rhi::IRHIDevice* device, u32 w, u32 h, bool enable);
    bool IsMSAAEnabled() const { return m_MSAAEnabled && m_MSAA != nullptr; }
    AA_MSAA* GetMSAA()         { return m_MSAA.get(); }

    // ── LDR 中间纹理 ──
    rhi::IRHITexture* GetLDRTarget()     const { return m_LDRTarget.get(); }
    rhi::IRHISampler* GetLDRSampler()    const { return m_LDRSampler.get(); }
    rhi::IRHITexture* GetLDRDummyDepth() const { return m_LDRDummyDepth.get(); }

private:
    // ── 后处理 Pass 实例 ──
    BloomPass        m_Bloom;
    DOFPass          m_DOF;
    MotionBlurPass   m_MotionBlur;
    AutoExposurePass m_AutoExposure;
    ColorGradingPass m_ColorGrading;
    CameraEffectsPass m_CameraEffects;
    std::unique_ptr<ToneMapPass>  m_ToneMap;
    std::unique_ptr<SkyboxPass>   m_Skybox;
    std::unique_ptr<IAntiAliasing> m_TAA;
    std::unique_ptr<AA_FXAA> m_FXAA;
    std::unique_ptr<AA_SMAA> m_SMAA;
    std::unique_ptr<AA_MSAA> m_MSAA;
    bool m_FXAAEnabled = false, m_SMAAEnabled = false, m_MSAAEnabled = false;

    // ── LDR 中间纹理 ──
    std::unique_ptr<rhi::IRHITexture> m_LDRTarget;
    std::unique_ptr<rhi::IRHISampler> m_LDRSampler;
    std::unique_ptr<rhi::IRHITexture> m_LDRDummyDepth;

    u32 m_Width = 0, m_Height = 0;
};

} // namespace he::render
