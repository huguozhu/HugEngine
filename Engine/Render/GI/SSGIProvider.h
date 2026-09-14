#pragma once

// ============================================================
// GI/SSGIProvider.h — 屏幕空间间接漫反射 Provider（Wave 2 阶段 2）
//
// SSGI 是「主 pass + 附属 pass（降噪）」的典型形态，用于验证附属 pass 机制：
//   · 主 pass：屏幕空间射线步进 → 间接漫反射（半分辨率可选）
//   · 附属 pass：时域/空间降噪（halfRes 时跳过——半分辨率输出直接采样）
// 帧图只需遍历 GetAuxPassCount()，不再为每种源手写降噪链。
// ============================================================

#include "GI/IGIProvider.h"
#include "GI/GI_SSGI.h"
#include "PostProcess/Denoiser.h"

namespace he::render {

/// 屏幕空间间接漫反射（中频）
class SSGIProvider final : public IGIProvider {
public:
    void SetPass(GI_SSGI* ssgi) { m_SSGI = ssgi; }
    void SetDenoiser(Denoiser* denoiser) { m_Denoise = denoiser; }
    /// 由帧图注入 GBuffer 输入
    void SetGBuffer(rhi::IRHITexture* depth, rhi::IRHITexture* normal, rhi::IRHITexture* albedo) {
        m_Depth = depth; m_Normal = normal; m_Albedo = albedo;
    }

    [[nodiscard]] GISourceId GetSourceId() const override { return GISourceId::SSGI; }
    [[nodiscard]] bool Handles(GISourceId id) const override { return id == GISourceId::SSGI; }
    [[nodiscard]] bool IsValid() const override { return m_SSGI && m_SSGI->IsEnabled(); }

    [[nodiscard]] rhi::IRHITexture* GetDiffuseOutput() const override {
        return m_SSGI ? m_SSGI->GetIndirectDiffuseTexture() : nullptr;
    }
    /// 最终输出：有降噪则取降噪结果，否则取主输出
    [[nodiscard]] rhi::IRHITexture* GetFinalDiffuseOutput() const override {
        if (AuxActive() && m_Denoise) return m_Denoise->GetOutput();
        return GetDiffuseOutput();
    }

    // ── 附属 pass：降噪（halfRes 时跳过）──
    [[nodiscard]] u32 GetAuxPassCount() const override { return AuxActive() ? 1u : 0u; }
    [[nodiscard]] const char* GetAuxPassName(u32 /*i*/) const override { return "SSGI_Denoise"; }
    [[nodiscard]] rhi::IRHITexture* GetAuxPassInput(u32 /*i*/) const override { return GetDiffuseOutput(); }
    [[nodiscard]] rhi::IRHITexture* GetAuxPassOutput(u32 /*i*/) const override {
        return m_Denoise ? m_Denoise->GetOutput() : nullptr;
    }
    void PreBindAux(rhi::IRHICommandList* cmd, u32 /*i*/) override {
        if (m_Denoise) m_Denoise->PreBind(cmd);
    }
    void RenderAux(rhi::IRHICommandList* cmd, u32 /*i*/, const GIProviderContext& /*ctx*/) override {
        if (m_Denoise && m_SSGI) {
            m_Denoise->SetInputs(GetDiffuseOutput(), m_Depth, m_Normal);
            m_Denoise->Render(cmd);
        }
    }

    bool Initialize(rhi::IRHIDevice*, u32, u32) override { return m_SSGI != nullptr; }
    void Shutdown() override {}
    void OnResize(u32, u32) override {}
    void PreBind(rhi::IRHICommandList* cmd) override { if (m_SSGI) m_SSGI->PreBind(cmd); }
    void Render(rhi::IRHICommandList* cmd, const GIProviderContext& /*ctx*/) override {
        if (m_SSGI) {
            m_SSGI->SetInputs(m_Depth, m_Normal, m_Albedo);
            m_SSGI->Render(cmd);
        }
    }

    [[nodiscard]] GI_SSGI*  GetPass() const { return m_SSGI; }
    [[nodiscard]] Denoiser* GetDenoiser() const { return m_Denoise; }
    [[nodiscard]] u32 GetOutputWidth() const {
        auto* t = GetDiffuseOutput(); return t ? t->GetWidth() : 0u;
    }
    [[nodiscard]] u32 GetOutputHeight() const {
        auto* t = GetDiffuseOutput(); return t ? t->GetHeight() : 0u;
    }

private:
    /// 降噪是否启用：halfRes 时半分辨率输出直接采样，省去 Denoise 开销
    [[nodiscard]] bool AuxActive() const {
        return m_SSGI && m_Denoise && !m_SSGI->GetSettings().halfRes;
    }

    GI_SSGI*  m_SSGI    = nullptr;   // 非拥有
    Denoiser* m_Denoise = nullptr;   // 非拥有
    rhi::IRHITexture* m_Depth  = nullptr;
    rhi::IRHITexture* m_Normal = nullptr;
    rhi::IRHITexture* m_Albedo = nullptr;
};

} // namespace he::render
