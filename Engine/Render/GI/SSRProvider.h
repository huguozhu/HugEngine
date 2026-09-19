#pragma once

// ============================================================
// GI/SSRProvider.h — 屏幕空间反射 Provider（Wave 2 阶段 3）
//
// 与 SSGIProvider 同构：主 pass（Hi-Z 层次追踪）+ 附属 pass（时域/空间降噪，
// halfRes 时跳过）。区别仅在输出通道不同（specular 而非 diffuse）。
// 两者共用同一套附属 pass 机制，说明该抽象对「屏幕空间 + 降噪」这一类源通用。
// ============================================================

#include "GI/IGIProvider.h"
#include "GI/GI_SSR.h"
#include "GI/SpatialDenoiseAux.h"   // 降噪附属 pass 的共享实现（任务 11.1）
#include "PostProcess/Denoiser.h"
#include "Core/Log.h"

namespace he::render {

/// 屏幕空间反射（中频）
class SSRProvider final : public IGIProvider {
public:
    void SetPass(GI_SSR* ssr) { m_SSR = ssr; }
    void SetDenoiser(Denoiser* denoiser) { m_Aux.SetPass(denoiser); }

    [[nodiscard]] GISourceId GetSourceId() const override { return GISourceId::SSR; }
    [[nodiscard]] bool Handles(GISourceId id) const override { return id == GISourceId::SSR; }
    [[nodiscard]] bool IsValid() const override { return m_SSR && m_SSR->IsEnabled(); }

    /// 同步到层栈：层栈是唯一真值（不变量 1）。必须在构图之前对齐 enabled，
    /// 否则「镜面层栈要求 SSR、SSR 子系统却关闭」会静默失效（IsValid 为假、pass 不注册）。
    /// 同时让 halfRes 当场生效（尺寸等下次 OnResize 才变会让本帧句柄指向旧纹理）。
    void SyncToStack(const GIChannelStack& stack) override {
        if (!m_SSR) return;
        const bool wanted = stack.Has(GISourceId::SSR);
        m_InStack = wanted;                       // 本帧是否真的产出（转储/信号登记都按它判定）
        m_SSR->SetEnabled(wanted);
        m_SSR->SyncOutputSize();
        // 【步骤 34（11.3）】两级降噪链的尺寸对齐（信号分辨率 / 消费端分辨率）
        rhi::IRHITexture* out = GetSpecularOutput();
        if (out) {
            m_Aux.SyncSizes(out->GetWidth(), out->GetHeight(),
                            m_SSR->GetFullWidth(), m_SSR->GetFullHeight());
        }
    }
    /// 本帧是否真的产出（见 `IGIProvider::ProducedThisFrame` 的说明）
    [[nodiscard]] bool ProducedThisFrame() const override { return IsValid() && m_InStack; }

    [[nodiscard]] rhi::IRHITexture* GetSpecularOutput() const override {
        return m_SSR ? m_SSR->GetIndirectSpecularTexture() : nullptr;
    }
    [[nodiscard]] rhi::IRHITexture* GetFinalSpecularOutput() const override {
        if (AuxActive()) return m_Aux.Output();
        return GetSpecularOutput();
    }

    // ── 附属 pass：降噪链（半分辨率时降噪 + 重建升采样）──
    // 与 SSGIProvider 共用 SpatialDenoiseAux（任务 11.1）：同一条链只有一份实现。
    [[nodiscard]] u32 GetAuxPassCount() const override { return m_Aux.Count(AuxActive()); }
    [[nodiscard]] const char* GetAuxPassName(u32 i) const override {
        return SpatialDenoiseAux::PassName(i, "SSR_Denoise", "SSR_Upscale");
    }
    [[nodiscard]] rhi::IRHITexture* GetAuxPassInput(u32 i) const override {
        return m_Aux.PassInput(i, GetSpecularOutput());
    }
    [[nodiscard]] rhi::IRHITexture* GetAuxPassOutput(u32 i) const override { return m_Aux.PassOutput(i); }
    void PreBindAux(rhi::IRHICommandList* cmd, u32 i) override { m_Aux.PreBind(cmd, i); }
    void RenderAux(rhi::IRHICommandList* cmd, u32 i, const GIProviderContext& /*ctx*/) override {
        m_Aux.Render(cmd, i, GetSpecularOutput(), m_Depth, m_Normal);
    }

    /// 由帧图注入 GBuffer 输入（SSR 需要反照率做降噪引导）
    void SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal,
                   rhi::IRHITexture* albedo) override {
        m_Depth = depth; m_Normal = normal; m_Albedo = albedo;
    }

    /// 【步骤 34（11.3）】升采样级需要设备（自有纹理/管线）
    bool Initialize(rhi::IRHIDevice* device, u32 w, u32 h) override {
        if (!m_SSR) return false;
        m_Aux.Initialize(device, w, h);
        return true;
    }
    void Shutdown() override { m_Aux.Shutdown(); }
    void OnResize(u32 w, u32 h) override { m_Aux.OnResize(w, h); }
    void PreBind(rhi::IRHICommandList* cmd) override { if (m_SSR) m_SSR->PreBind(cmd); }
    void Render(rhi::IRHICommandList* cmd, const GIProviderContext& ctx) override {
        if (m_SSR) {
            m_SSR->SetInputs(m_Depth, m_Normal, m_Albedo);
            // 屏幕空间重建/回投影必须用渲染深度图的那套投影参数（§9.2-E）
            m_SSR->SetCamera(ctx.camera);
            m_SSR->Render(cmd);
        }
    }

    [[nodiscard]] GI_SSR* GetPass() const { return m_SSR; }
    [[nodiscard]] Denoiser* GetDenoiser() const { return m_Aux.GetPass(); }
    /// 计时读数落点（任务 29 / §9.2-Z）：帧图的 GPU 计时器把测得的耗时写回它
    [[nodiscard]] IGlobalIllumination* GetTimedPass() const override { return m_SSR; }

    /// 【步骤 34（11.3）】把 SSR 的降噪信号登记进统一框架：与 SSGI 同构，
    /// 区别只在输出通道是 specular。halfRes 时同样是「半分辨率也降噪 + 之后升采样」。
    void DescribeSignals(DenoiseSignalRegistry& registry, rhi::IRHITexture* depth,
                         rhi::IRHITexture* normal, rhi::IRHITexture* velocity) override {
        if (!m_SSR || !m_SSR->IsEnabled()) return;
        rhi::IRHITexture* main = GetSpecularOutput();
        if (!main) return;
        const bool halfRes = m_SSR->GetSettings().halfRes;
        DenoiseSignal s;
        s.name         = "SSR";
        s.input        = main;
        s.output       = AuxActive() ? m_Aux.Output() : main;
        s.depth        = depth;
        s.normal       = normal;
        s.velocity     = velocity;
        s.width        = main->GetWidth();
        s.height       = main->GetHeight();
        s.targetWidth  = halfRes ? m_SSR->GetFullWidth()  : s.width;
        s.targetHeight = halfRes ? m_SSR->GetFullHeight() : s.height;
        s.needsUpscale = AuxActive() && m_Aux.NeedsUpscale();
        s.depthSigma   = m_Aux.GetDepthSigma();
        s.normalSigma  = m_Aux.GetNormalSigma();
        registry.Register(s);
    }

private:
    /// 降噪链是否启用：只看降噪器是否就绪 —— 半分辨率也照跑（步骤 34 / 11.3）
    [[nodiscard]] bool AuxActive() const {
        return m_SSR && m_Aux.Active();
    }

    GI_SSR*           m_SSR = nullptr;   // 非拥有
    SpatialDenoiseAux m_Aux;             // 降噪附属 pass（与 SSGIProvider 共用实现）
    bool              m_InStack = false; // 本帧层栈是否要求了 SSR（由 SyncToStack 写入）
    rhi::IRHITexture* m_Depth  = nullptr;
    rhi::IRHITexture* m_Normal = nullptr;
    rhi::IRHITexture* m_Albedo = nullptr;
};

} // namespace he::render
