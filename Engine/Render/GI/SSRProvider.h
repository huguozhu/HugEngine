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
#include "PostProcess/Denoiser.h"
#include "Core/Log.h"

namespace he::render {

/// 屏幕空间反射（中频）
class SSRProvider final : public IGIProvider {
public:
    void SetPass(GI_SSR* ssr) { m_SSR = ssr; }
    void SetDenoiser(Denoiser* denoiser) { m_Denoise = denoiser; }

    [[nodiscard]] GISourceId GetSourceId() const override { return GISourceId::SSR; }
    [[nodiscard]] bool Handles(GISourceId id) const override { return id == GISourceId::SSR; }
    [[nodiscard]] bool IsValid() const override { return m_SSR && m_SSR->IsEnabled(); }

    /// 同步到层栈：层栈是唯一真值（不变量 1）。必须在构图之前对齐 enabled，
    /// 否则「镜面层栈要求 SSR、SSR 子系统却关闭」会静默失效（IsValid 为假、pass 不注册）。
    /// 同时让 halfRes 当场生效（尺寸等下次 OnResize 才变会让本帧句柄指向旧纹理）。
    void SyncToStack(const GIChannelStack& stack) override {
        if (!m_SSR) return;
        m_SSR->SetEnabled(stack.Has(GISourceId::SSR));
        m_SSR->SyncOutputSize();
    }

    [[nodiscard]] rhi::IRHITexture* GetSpecularOutput() const override {
        return m_SSR ? m_SSR->GetIndirectSpecularTexture() : nullptr;
    }
    [[nodiscard]] rhi::IRHITexture* GetFinalSpecularOutput() const override {
        if (AuxActive() && m_Denoise) return m_Denoise->GetOutput();
        return GetSpecularOutput();
    }

    // ── 附属 pass：降噪（halfRes 时跳过）──
    [[nodiscard]] u32 GetAuxPassCount() const override { return AuxActive() ? 1u : 0u; }
    [[nodiscard]] const char* GetAuxPassName(u32 /*i*/) const override { return "SSR_Denoise"; }
    [[nodiscard]] rhi::IRHITexture* GetAuxPassInput(u32 /*i*/) const override { return GetSpecularOutput(); }
    [[nodiscard]] rhi::IRHITexture* GetAuxPassOutput(u32 /*i*/) const override {
        return m_Denoise ? m_Denoise->GetOutput() : nullptr;
    }
    void PreBindAux(rhi::IRHICommandList* cmd, u32 /*i*/) override {
        if (m_Denoise) m_Denoise->PreBind(cmd);
    }
    void RenderAux(rhi::IRHICommandList* cmd, u32 /*i*/, const GIProviderContext& /*ctx*/) override {
        if (m_Denoise && m_SSR) {
            m_Denoise->SetInputs(GetSpecularOutput(), m_Depth, m_Normal);
            m_Denoise->Render(cmd);
        }
    }

    /// 由帧图注入 GBuffer 输入（SSR 需要反照率做降噪引导）
    void SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal,
                   rhi::IRHITexture* albedo) override {
        m_Depth = depth; m_Normal = normal; m_Albedo = albedo;
    }

    bool Initialize(rhi::IRHIDevice*, u32, u32) override { return m_SSR != nullptr; }
    void Shutdown() override {}
    void OnResize(u32, u32) override {}
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

private:
    [[nodiscard]] bool AuxActive() const {
        return m_SSR && m_Denoise && !m_SSR->GetSettings().halfRes;
    }

    GI_SSR*   m_SSR     = nullptr;   // 非拥有
    Denoiser* m_Denoise = nullptr;   // 非拥有
    rhi::IRHITexture* m_Depth  = nullptr;
    rhi::IRHITexture* m_Normal = nullptr;
    rhi::IRHITexture* m_Albedo = nullptr;
};

} // namespace he::render
