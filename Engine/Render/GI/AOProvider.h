#pragma once

// ============================================================
// GI/AOProvider.h — 屏幕空间 AO 的 Provider（P4 / Wave 2 试点）
//
// SSAO 与 GTAO 是同类互斥算法（都估计「环境光遮蔽」这一物理量），共用同一个
// SSAO pass（切换片段着色器）。因此一个 Provider 同时代表这两个源：
//   · GetSourceId()  返回「当前生效」的源（层栈要求 GTAO 时即 GTAO）
//   · Handles(id)    对 SSAO / GTAO 都返回 true（面板候选与 NeedsPass 匹配）
//
// 试点意义：验证「新增 GI 只需实现 Provider + 注册」这一承诺——
// 帧图不再手写 `ShouldRunSSAO()/useGTAO = ...`，而是遍历注册表。
// ============================================================

#include "GI/IGIProvider.h"
#include "PostProcess/SSAO.h"

namespace he::render {

/// 屏幕空间 AO Provider（SSAO / GTAO 共用同一 pass）
class ScreenAOProvider final : public IGIProvider {
public:
    /// 注入底层 pass（管线持有所有权，Provider 只引用）
    void SetPass(SSAO* pass) { m_Pass = pass; }

    [[nodiscard]] GISourceId GetSourceId() const override {
        return (m_Pass && m_Pass->useGTAO) ? GISourceId::GTAO : GISourceId::SSAO;
    }
    [[nodiscard]] bool Handles(GISourceId id) const override {
        return id == GISourceId::SSAO || id == GISourceId::GTAO;
    }
    [[nodiscard]] bool IsValid() const override {
        return m_Pass != nullptr && m_Pass->enabled;
    }
    /// 层栈含 SSAO 或 GTAO 任一 → 需要本 pass
    [[nodiscard]] bool NeedsPass(const GIChannelStack& stack) const override {
        return (stack.Has(GISourceId::SSAO) || stack.Has(GISourceId::GTAO)) && IsValid();
    }
    /// 同步到层栈：层栈是唯一真值。
    /// 除了切 GTAO/SSAO 模式，还必须把子系统 enabled 也对齐 —— 层栈说参与，子系统就得
    /// 真的启用。此前只切模式、开关留给调用方设置，于是「层栈要求参与、子系统却关闭」会
    /// 静默失效（Provider::IsValid 为假、pass 不注册，见 §9.2-G）。
    void SyncToStack(const GIChannelStack& stack) override {
        if (!m_Pass) return;
        m_Pass->enabled = stack.Has(GISourceId::SSAO) || stack.Has(GISourceId::GTAO);
        m_Pass->useGTAO = stack.Has(GISourceId::GTAO);
    }

    [[nodiscard]] rhi::IRHITexture* GetAOOutput() const override {
        return m_Pass ? m_Pass->GetAOTexture() : nullptr;
    }

    // 底层 pass 的生命周期由管线管理（Initialize/Shutdown/OnResize 均透传为无操作）
    bool Initialize(rhi::IRHIDevice*, u32, u32) override { return m_Pass != nullptr; }
    void Shutdown() override {}
    void OnResize(u32, u32) override {}
    void Render(rhi::IRHICommandList* cmd, const GIProviderContext& /*ctx*/) override {
        if (m_Pass) m_Pass->Render(cmd);
    }
    void PreBind(rhi::IRHICommandList* cmd) override { if (m_Pass) m_Pass->PreBind(cmd); }
    void SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal,
                   rhi::IRHITexture* /*albedo*/) override {
        if (m_Pass) m_Pass->SetInputs(depth, normal);
    }

    /// 底层 pass 访问（仅供管线/面板取参数；帧图通过接口方法访问，无需 dynamic_cast）
    [[nodiscard]] SSAO* GetPass() const { return m_Pass; }

private:
    SSAO* m_Pass = nullptr;   // 非拥有
};

} // namespace he::render
