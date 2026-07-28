// ============================================================
// PostProcessChain.cpp — 后处理责任链实现
// 统一管理所有后处理 Pass 的生命周期
// ============================================================
#include "PostProcess/PostProcessChain.h"
#include "AntiAliasing/AA_TAA.h"
#include "Core/Log.h"

namespace he::render {

void PostProcessChain::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Width  = width;
    m_Height = height;

    // ── 立即初始化的 Pass ──
    m_ToneMap = std::make_unique<ToneMapPass>();
    m_ToneMap->Initialize(device, width, height);

    m_Skybox = std::make_unique<SkyboxPass>();
    m_Skybox->Initialize(device, width, height);

    // TAA（HDR 空间）
    m_TAA = std::make_unique<AA_TAA>();
    if (!m_TAA->Initialize(device, width, height)) {
        HE_CORE_WARN("PostProcessChain: TAA init failed, disabled");
        m_TAA.reset();
    }

    m_AutoExposure.Initialize(device, width, height);

    // ── LDR 中间纹理（ToneMap → LDR → FXAA/SMAA/ColorGrading）──
    {
        rhi::TextureDesc d;
        d.format = rhi::Format::BGRA8_UNORM;
        d.width  = width; d.height = height;
        d.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        m_LDRTarget = device->CreateTexture(d);

        rhi::SamplerDesc s;
        s.minFilter = s.magFilter = rhi::FilterMode::Linear;
        s.addressU = s.addressV = rhi::AddressMode::ClampToEdge;
        m_LDRSampler = device->CreateSampler(s);

        // 虚拟深度附件（ToneMap PSO 带 depthFormat，Offscreen 需 2 附件）
        // 必须与 LDR target 同尺寸，否则 vkCreateFramebuffer 因附件尺寸不匹配失败
        rhi::TextureDesc dd;
        dd.format = rhi::Format::D32_FLOAT;
        dd.width  = width; dd.height = height;
        dd.usage  = rhi::TextureUsage::DepthStencil;
        m_LDRDummyDepth = device->CreateTexture(dd);
    }

    // ── 懒初始化的 Pass（Bloom/DOF/MotionBlur/ColorGrading/FXAA/SMAA/MSAA）──
    // 首次调用 SetEnabled(true) 时才分配 GPU 资源

    HE_CORE_INFO("PostProcessChain: 初始化完成 ({}x{})", width, height);
}

void PostProcessChain::Shutdown() {
    m_Bloom.Shutdown();
    m_DOF.Shutdown();
    m_MotionBlur.Shutdown();
    m_AutoExposure.Shutdown();
    m_ColorGrading.Shutdown();
    if (m_ToneMap) m_ToneMap->Shutdown();
    if (m_Skybox)  m_Skybox->Shutdown();
    if (m_TAA)     m_TAA->Shutdown();
    m_TAA.reset();
    if (m_FXAA)    m_FXAA->Shutdown();
    m_FXAA.reset();
    if (m_SMAA)    m_SMAA->Shutdown();
    m_SMAA.reset();
    if (m_MSAA)    m_MSAA->Shutdown();
    m_MSAA.reset();
    m_LDRTarget.reset();
    m_LDRSampler.reset();
    m_LDRDummyDepth.reset();
    m_Width = m_Height = 0;
}

void PostProcessChain::OnResize(rhi::IRHIDevice* device, u32 width, u32 height) {
    if (width == m_Width && height == m_Height) return;
    m_Width  = width;
    m_Height = height;

    if (m_ToneMap) m_ToneMap->OnResize(width, height);
    if (m_Skybox)  { m_Skybox->Shutdown(); m_Skybox->Initialize(device, width, height); }
    if (m_TAA)     m_TAA->OnResize(width, height);
    if (m_FXAA)    m_FXAA->OnResize(width, height);
    if (m_SMAA)    m_SMAA->OnResize(width, height);

    // 重建 LDR 中间纹理
    {
        rhi::TextureDesc d;
        d.format = rhi::Format::BGRA8_UNORM;
        d.width  = width; d.height = height;
        d.usage  = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
        m_LDRTarget = device->CreateTexture(d);
    }
    {
        rhi::TextureDesc dd;
        dd.format = rhi::Format::D32_FLOAT;
        dd.width  = width; dd.height = height;
        dd.usage  = rhi::TextureUsage::DepthStencil;
        m_LDRDummyDepth = device->CreateTexture(dd);
    }

    m_Bloom.OnResize(width, height);
    m_AutoExposure.OnResize(width, height);
}

void PostProcessChain::EnableFXAA(rhi::IRHIDevice* device, u32 w, u32 h, bool enable) {
    m_FXAAEnabled = enable;
    if (!enable || !device) return;
    if (!m_FXAA) {
        m_FXAA = std::make_unique<AA_FXAA>();
        if (!m_FXAA->Initialize(device, w, h)) {
            HE_CORE_WARN("PostProcessChain: FXAA init failed");
            m_FXAA.reset();
        }
    }
}

void PostProcessChain::EnableSMAA(rhi::IRHIDevice* device, u32 w, u32 h, bool enable) {
    m_SMAAEnabled = enable;
    if (!enable || !device) return;
    if (!m_SMAA) {
        m_SMAA = std::make_unique<AA_SMAA>();
        if (!m_SMAA->Initialize(device, w, h)) {
            HE_CORE_WARN("PostProcessChain: SMAA init failed");
            m_SMAA.reset();
        }
    }
}

void PostProcessChain::EnableMSAA(rhi::IRHIDevice* device, u32 w, u32 h, bool enable) {
    m_MSAAEnabled = enable;
    if (!enable || !device) return;
    if (!m_MSAA) {
        m_MSAA = std::make_unique<AA_MSAA>();
        m_MSAA->Initialize(device, w, h);
    }
}

} // namespace he::render
