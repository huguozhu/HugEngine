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
#include "GI/SpatialDenoiseAux.h"   // 降噪附属 pass 的共享实现（任务 11.1）
#include "PostProcess/Denoiser.h"

namespace he::render {

/// 屏幕空间间接漫反射（中频）
class SSGIProvider final : public IGIProvider {
public:
    void SetPass(GI_SSGI* ssgi) { m_SSGI = ssgi; }
    void SetDenoiser(Denoiser* denoiser) { m_Aux.SetPass(denoiser); }
    /// 由帧图注入 GBuffer 输入
    /// 【必须叫 SetInputs 且显式 override】帧图统一调用接口方法 IGIProvider::SetInputs，
    /// 而该虚函数带**空实现的默认体**：方法改名（此处曾叫 SetGBuffer）不会触发任何编译
    /// 错误，只会静默落到空实现 → m_Depth/m_Normal/m_Albedo 恒为 nullptr →
    /// GI_SSGI::Render 在守卫处直接 return → SSGI 输出纹理只剩清屏值，
    /// 表现为「SSGI 已启用、诊断也认为有效，但对画面的贡献恒为 0」。
    void SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal,
                   rhi::IRHITexture* albedo) override {
        m_Depth = depth; m_Normal = normal; m_Albedo = albedo;
    }

    [[nodiscard]] GISourceId GetSourceId() const override { return GISourceId::SSGI; }
    [[nodiscard]] bool Handles(GISourceId id) const override { return id == GISourceId::SSGI; }
    [[nodiscard]] bool IsValid() const override { return m_SSGI && m_SSGI->IsEnabled(); }

    /// SSGI 的入射辐射度 L_in 取自「前帧 HDR 辐射度」共享组件（§9.2-P）→ 捕获门控必须把它
    /// 算进消费者。此前帧图只按 DDGI 判断，于是「diffuse = {SSGI}」时从不捕获，
    /// 本源采样到一张从未写入的纹理、输出恒为 0（与 §9.2-Q 同一类"消费者门控写漏"）。
    [[nodiscard]] bool NeedsRadianceHistory() const override {
        return m_SSGI != nullptr && m_SSGI->IsEnabled();
    }

    /// 同步到层栈：层栈是唯一真值（不变量 1）。
    /// 调用点由帧图在**构图之前**调用，因此这里也是让 halfRes 当场生效的正确时机 ——
    /// 输出纹理尺寸若等到下次 OnResize 才变，本帧导入渲染图的句柄就会指向旧尺寸纹理。
    void SyncToStack(const GIChannelStack& stack) override {
        if (!m_SSGI) return;
        m_SSGI->SetEnabled(stack.Has(GISourceId::SSGI));
        m_SSGI->SyncOutputSize();
    }

    [[nodiscard]] rhi::IRHITexture* GetDiffuseOutput() const override {
        return m_SSGI ? m_SSGI->GetIndirectDiffuseTexture() : nullptr;
    }
    /// 最终输出：有降噪则取降噪结果，否则取主输出
    [[nodiscard]] rhi::IRHITexture* GetFinalDiffuseOutput() const override {
        if (AuxActive()) return m_Aux.Output();
        return GetDiffuseOutput();
    }

    // ── 附属 pass：降噪（halfRes 时跳过）──
    // 实现全部委托给 SpatialDenoiseAux：与 SSRProvider 共用同一条链，避免两处逐行同构
    // 的代码各自漂移（任务 11.1）。
    [[nodiscard]] u32 GetAuxPassCount() const override { return m_Aux.Count(AuxActive()); }
    [[nodiscard]] const char* GetAuxPassName(u32 /*i*/) const override { return "SSGI_Denoise"; }
    [[nodiscard]] rhi::IRHITexture* GetAuxPassInput(u32 /*i*/) const override { return GetDiffuseOutput(); }
    [[nodiscard]] rhi::IRHITexture* GetAuxPassOutput(u32 /*i*/) const override { return m_Aux.Output(); }
    void PreBindAux(rhi::IRHICommandList* cmd, u32 /*i*/) override { m_Aux.PreBind(cmd); }
    void RenderAux(rhi::IRHICommandList* cmd, u32 /*i*/, const GIProviderContext& /*ctx*/) override {
        m_Aux.Render(cmd, GetDiffuseOutput(), m_Depth, m_Normal);
    }

    bool Initialize(rhi::IRHIDevice*, u32, u32) override { return m_SSGI != nullptr; }
    void Shutdown() override {}
    void OnResize(u32, u32) override {}
    void PreBind(rhi::IRHICommandList* cmd) override { if (m_SSGI) m_SSGI->PreBind(cmd); }
    void Render(rhi::IRHICommandList* cmd, const GIProviderContext& ctx) override {
        if (m_SSGI) {
            // ctx 必须消费：屏幕空间重建要用渲染深度图时的那套相机参数（§9.2-E）。
            // 此前这里把 ctx 整个忽略（形参写作 /*ctx*/），SSGI 只能自力拼默认投影矩阵。
            m_SSGI->SetCamera(ctx.camera);
            // 白炉条件（全白环境 + albedo=1）也必须传给本 pass：只靠 Lighting 侧短路的话，
            // 白炉判据就不覆盖 SSGI 的标度（§11.4 的风险项）。
            m_SSGI->SetFurnaceMode(ctx.furnace);
            m_SSGI->SetInputs(m_Depth, m_Normal, m_Albedo);
            m_SSGI->Render(cmd);
        }
    }

    [[nodiscard]] GI_SSGI*  GetPass() const { return m_SSGI; }
    [[nodiscard]] Denoiser* GetDenoiser() const { return m_Aux.GetPass(); }
    [[nodiscard]] u32 GetOutputWidth() const {
        auto* t = GetDiffuseOutput(); return t ? t->GetWidth() : 0u;
    }
    [[nodiscard]] u32 GetOutputHeight() const {
        auto* t = GetDiffuseOutput(); return t ? t->GetHeight() : 0u;
    }

    /// 计时读数落点（任务 29 / §9.2-Z）：帧图的 GPU 计时器把测得的耗时写回它
    [[nodiscard]] IGlobalIllumination* GetTimedPass() const override { return m_SSGI; }

private:
    /// 降噪是否启用：halfRes 时半分辨率输出直接采样，省去 Denoise 开销
    [[nodiscard]] bool AuxActive() const {
        return m_SSGI && m_Aux.GetPass() && !m_SSGI->GetSettings().halfRes;
    }

    GI_SSGI*          m_SSGI = nullptr;   // 非拥有
    SpatialDenoiseAux m_Aux;              // 降噪附属 pass（与 SSRProvider 共用实现）
    rhi::IRHITexture* m_Depth  = nullptr;
    rhi::IRHITexture* m_Normal = nullptr;
    rhi::IRHITexture* m_Albedo = nullptr;
};

} // namespace he::render
