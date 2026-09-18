#pragma once

// ============================================================
// GI/RTProvider.h — 光追效果 Provider（Wave 2 阶段 5）
//
// 四个 RT 效果（RT 阴影 / RTAO / RT 反射 / RTGI）结构相同：
//   主 pass（对 GBuffer 像素发射射线）→ 时域累积降噪 →（反射/GI 追加空间滤波）
// 因此用一个参数化的 Provider 覆盖四种效果，避免四份重复代码。
//
// 两处与屏幕空间源不同的地方：
//   · 依赖共享的加速结构：由 ASBuildProvider 先行构建 TLAS（注册顺序保证）
//   · RT 阴影不属于三个层栈通道（阴影是独立枚举）→ 覆写 NeedsPass 判断
// ============================================================

#include "GI/IGIProvider.h"
#include "RT/RTShadowPass.h"
#include "RT/RTAOPass.h"
#include "RT/RTReflectionPass.h"
#include "RT/RTGIPass.h"
#include "RT/RTEffectPass.h"
#include "PostProcess/RTDenoiser.h"
#include "PostProcess/Denoiser.h"
#include "Pipeline/RTPass.h"

namespace he::render {

/// 光追效果的 Provider（四种效果共用一个实现）
class RTEffectProvider final : public IGIProvider {
public:
    enum class Effect : u8 { Shadow, AO, Reflection, GI };

    explicit RTEffectProvider(Effect effect) : m_Effect(effect) {}

    // ── 注入底层 pass（均非拥有，由管线管理生命周期）──
    void SetAS(RTPass* as) { m_AS = as; }
    void SetShadowPass(RTShadowPass* pass, RTDenoiser* temporal) {
        m_Shadow = pass; m_Temporal = temporal;
    }
    void SetAOPass(RTAOPass* pass, RTDenoiser* temporal) {
        m_AO = pass; m_Temporal = temporal;
    }
    void SetReflectionPass(RTReflectionPass* pass, RTDenoiser* temporal, Denoiser* spatial) {
        m_Reflection = pass; m_Temporal = temporal; m_Spatial = spatial;
    }
    void SetGIPass(RTGIPass* pass, RTDenoiser* temporal, Denoiser* spatial) {
        m_GI = pass; m_Temporal = temporal; m_Spatial = spatial;
    }
    /// RTGI 的 miss 回退需要 DDGI 探针（由帧图注入）
    void SetDDGIFallback(rhi::IRHIBuffer* probeBuffer, rhi::IRHIBuffer* gridUniform) {
        m_DDGIProbe = probeBuffer; m_DDGIGrid = gridUniform;
    }

    // ── 身份 ──
    /// RT 阴影的「源」不在 GI 层栈内（阴影是独立枚举，不进层栈）→ 返回 None。
    /// 帧图对本 Provider 单独以 ShouldRunRTShadow() 判断（见 NeedsPass）。
    [[nodiscard]] GISourceId GetSourceId() const override {
        switch (m_Effect) {
        case Effect::Shadow:     return GISourceId::None;
        case Effect::AO:         return GISourceId::RTAO;
        case Effect::Reflection: return GISourceId::RTReflection;
        default:                 return GISourceId::RTGI;
        }
    }
    [[nodiscard]] bool Handles(GISourceId id) const override { return id == GetSourceId(); }
    [[nodiscard]] bool IsValid() const override { return MainPassValid(); }

    /// RT 阴影不属于层栈的通道（阴影为独立枚举）→ 单独判断
    [[nodiscard]] bool NeedsPass(const GIChannelStack& stack) const override {
        if (!IsValid()) return false;
        if (m_Effect == Effect::Shadow) return m_RTShadowWanted;
        return stack.Has(GetSourceId());
    }
    /// 由帧图每帧告知「阴影枚举当前是否选择 RT 阴影」
    void SetRTShadowWanted(bool wanted) { m_RTShadowWanted = wanted; }
    [[nodiscard]] bool IsShadowEffect() const { return m_Effect == Effect::Shadow; }
    /// pass 名：RT 阴影不在 GI 源枚举内（阴影是独立枚举），单独给名
    [[nodiscard]] const char* GetName() const override {
        switch (m_Effect) {
        case Effect::Shadow:     return "RT Shadow";
        case Effect::AO:         return "RTAO";
        case Effect::Reflection: return "RT Reflection";
        default:                 return "RTGI";
        }
    }
    /// RT 阴影输出（不在层栈三通道内，供帧图单独取给 Lighting）
    [[nodiscard]] rhi::IRHITexture* GetShadowOutput() const {
        if (m_Effect != Effect::Shadow) return nullptr;
        return HasTemporal() ? TemporalOutput() : MainOutput();
    }

    // ── 通道输出 ──
    [[nodiscard]] rhi::IRHITexture* GetDiffuseOutput() const override {
        return m_Effect == Effect::GI ? m_GI->GetOutput() : nullptr;
    }
    [[nodiscard]] rhi::IRHITexture* GetSpecularOutput() const override {
        return m_Effect == Effect::Reflection ? m_Reflection->GetOutput() : nullptr;
    }
    [[nodiscard]] rhi::IRHITexture* GetAOOutput() const override {
        return m_Effect == Effect::AO ? m_AO->GetOutput() : nullptr;
    }

    // ── 附属 pass：时域累积（+ 反射/GI 的空间滤波）──
    // 索引约定：0 = 时域（存在时），其后 = 空间滤波；时域缺失时空间滤波前移为 0
    [[nodiscard]] u32 GetAuxPassCount() const override {
        return (HasTemporal() ? 1u : 0u) + (HasSpatialPass() ? 1u : 0u);
    }
    [[nodiscard]] const char* GetAuxPassName(u32 i) const override {
        return IsTemporalIndex(i) ? TemporalName() : SpatialName();
    }
    [[nodiscard]] rhi::IRHITexture* GetAuxPassInput(u32 i) const override {
        return IsTemporalIndex(i) ? MainOutput() : TemporalOrMain();
    }
    [[nodiscard]] rhi::IRHITexture* GetAuxPassOutput(u32 i) const override {
        return IsTemporalIndex(i) ? TemporalOutput() : SpatialOutput();
    }
    [[nodiscard]] rhi::IRHITexture* GetFinalDiffuseOutput() const override {
        return m_Effect == Effect::GI ? FinalOutput() : nullptr;
    }
    [[nodiscard]] rhi::IRHITexture* GetFinalSpecularOutput() const override {
        return m_Effect == Effect::Reflection ? FinalOutput() : nullptr;
    }
    [[nodiscard]] rhi::IRHITexture* GetFinalAOOutput() const override {
        return m_Effect == Effect::AO ? FinalOutput() : nullptr;
    }

    void PreBindAux(rhi::IRHICommandList* cmd, u32 i) override {
        // 【两者都必须在这里绑管线】帧图的附属 pass 会紧接着调 BeginOffscreenPass，而它是用
        // 「当前绑定的 PSO」去取 RenderPass 建 Framebuffer 的。若此刻还绑着上一 Pass 的 PSO
        // （例如 RTGI 的光线追踪管线、或带深度附件的图形 PSO），Framebuffer 附件数就会与
        // RenderPass 不匹配：校验层报 VUID-VkFramebufferCreateInfo-attachmentCount-00876，
        // 实测更严重 —— 设备直接挂住，进程再也不会推进（启用 RTGI 时稳定复现）。
        if (IsTemporalIndex(i)) { if (m_Temporal) m_Temporal->PreBind(cmd); }
        else if (m_Spatial)     { m_Spatial->PreBind(cmd); }
    }
    void RenderAux(rhi::IRHICommandList* cmd, u32 i, const GIProviderContext& /*ctx*/) override {
        if (IsTemporalIndex(i)) {
            if (m_Temporal) {
                m_Temporal->SetInputs(MainOutput(), m_Depth, m_Normal, m_Velocity);
                m_Temporal->Render(cmd);
            }
        } else if (m_Spatial) {
            m_Spatial->SetInputs(TemporalOrMain(), m_Depth, m_Normal);
            m_Spatial->Render(cmd);
        }
    }

    // ── 生命周期 ──
    bool Initialize(rhi::IRHIDevice*, u32, u32) override { return IsValid(); }
    void Shutdown() override {}
    void OnResize(u32, u32) override {}
    void SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal,
                   rhi::IRHITexture* albedo) override {
        m_Depth = depth; m_Normal = normal; m_Albedo = albedo;
    }
    void SetVelocity(rhi::IRHITexture* velocity) { m_Velocity = velocity; }

    /// 主 pass：向 GBuffer 有效像素发射射线
    void Render(rhi::IRHICommandList* cmd, const GIProviderContext& ctx) override;

private:
    [[nodiscard]] bool HasSpatial() const {
        return m_Effect == Effect::Reflection || m_Effect == Effect::GI;
    }
    [[nodiscard]] bool HasTemporal() const { return m_Temporal && m_Temporal->IsReady(); }
    [[nodiscard]] bool HasSpatialPass() const { return HasSpatial() && m_Spatial && m_Spatial->IsReady(); }
    /// 索引 i 是否为「时域」附属 pass（时域缺失时全部是空间）
    [[nodiscard]] bool IsTemporalIndex(u32 i) const { return HasTemporal() && i == 0u; }
    [[nodiscard]] rhi::IRHITexture* TemporalOrMain() const {
        rhi::IRHITexture* t = TemporalOutput();
        return t ? t : MainOutput();
    }
    [[nodiscard]] rhi::IRHITexture* SpatialOutput() const {
        return m_Spatial ? m_Spatial->GetOutput() : nullptr;
    }
    [[nodiscard]] bool MainPassValid() const {
        switch (m_Effect) {
        case Effect::Shadow:     return m_Shadow != nullptr;
        case Effect::AO:         return m_AO != nullptr;
        case Effect::Reflection: return m_Reflection != nullptr;
        default:                 return m_GI != nullptr;
        }
    }
    [[nodiscard]] rhi::IRHITexture* MainOutput() const {
        switch (m_Effect) {
        case Effect::Shadow:     return m_Shadow->GetOutput();
        case Effect::AO:         return m_AO->GetOutput();
        case Effect::Reflection: return m_Reflection->GetOutput();
        default:                 return m_GI->GetOutput();
        }
    }
    [[nodiscard]] rhi::IRHITexture* TemporalOutput() const {
        return m_Temporal ? m_Temporal->GetOutput() : nullptr;
    }
    /// 最终输出：空间滤波 → 时域累积 → 主输出
    [[nodiscard]] rhi::IRHITexture* FinalOutput() const {
        if (HasSpatialPass()) return SpatialOutput();
        if (HasTemporal()) return TemporalOutput();
        return MainOutput();
    }
    [[nodiscard]] const char* TemporalName() const {
        switch (m_Effect) {
        case Effect::Shadow:     return "RT_Shadow_Denoise";
        case Effect::AO:         return "RT_AO_Denoise";
        case Effect::Reflection: return "RT_Reflection_Temporal";
        default:                 return "RT_GI_Temporal";
        }
    }
    [[nodiscard]] const char* SpatialName() const {
        return m_Effect == Effect::Reflection ? "RT_Reflection_Denoise" : "RT_GI_Denoise";
    }

    Effect m_Effect;
    bool   m_RTShadowWanted = false;

    RTPass*           m_AS         = nullptr;
    RTShadowPass*     m_Shadow     = nullptr;
    RTAOPass*         m_AO         = nullptr;
    RTReflectionPass* m_Reflection = nullptr;
    RTGIPass*         m_GI         = nullptr;
    RTDenoiser*       m_Temporal   = nullptr;
    Denoiser*         m_Spatial    = nullptr;

    rhi::IRHITexture* m_Depth    = nullptr;
    rhi::IRHITexture* m_Normal   = nullptr;
    rhi::IRHITexture* m_Albedo   = nullptr;
    rhi::IRHITexture* m_Velocity = nullptr;
    rhi::IRHIBuffer*  m_DDGIProbe = nullptr;
    rhi::IRHIBuffer*  m_DDGIGrid  = nullptr;
};

// ── 主 pass 实现（向 GBuffer 有效像素发射射线）──
// 四个效果的 Execute 调用形式一致，仅 pass 类型不同；
// 场景材质纹理、三角形法线、TLAS 均取自共享的 RTPass。
inline void RTEffectProvider::Render(rhi::IRHICommandList* cmd, const GIProviderContext& ctx) {
    if (!ctx.camera || !m_AS || !MainPassValid()) return;

    RTExecuteContext rc;
    rc.invViewProj = glm::inverse(ctx.camera->GetViewProjMatrix());
    rc.cameraPos   = ctx.camera->position;
    rc.frameIndex  = ctx.frameIndex;
    rc.gbDepth     = m_Depth;
    rc.gbNormal    = m_Normal;
    rc.lightBuffer = ctx.lightBuffer;
    rc.lightCount  = ctx.lightCount;
    rc.sceneMaterialTex     = m_AS->GetSceneMaterialTexture();
    rc.sceneTriangleNormals = m_AS->GetSceneTriangleNormals();
    rc.ddgiProbeBuffer = m_DDGIProbe;   // GI 的 miss 回退：DDGI 探针
    rc.ddgiGridUniform = m_DDGIGrid;

    rhi::IRHIAccelerationStructure* tlas = ctx.tlas ? ctx.tlas : m_AS->GetTLAS();
    switch (m_Effect) {
    case Effect::Shadow:     m_Shadow->Execute(cmd, tlas, rc);     break;
    case Effect::AO:         m_AO->Execute(cmd, tlas, rc);         break;
    case Effect::Reflection: m_Reflection->Execute(cmd, tlas, rc); break;
    default:                 m_GI->Execute(cmd, tlas, rc);         break;
    }
}

} // namespace he::render
