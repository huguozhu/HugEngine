#pragma once

#include "AntiAliasing.h"

namespace he::render {

// ============================================================
// AA_None — 空对象（Null Object 模式）
//
// 所有 AA 操作均为 no-op，管线透传原始 HDR 纹理给下游。
// IsEnabled() 返回 false，ImGui 面板可据此隐藏 AA 控件。
// ============================================================
class AA_None final : public IAntiAliasing {
public:
    AA_None()  = default;
    ~AA_None() override = default;

    // ---- IRenderSubsystem ----

    bool Initialize(rhi::IRHIDevice*, u32, u32) override {
        m_Ready = true;
        return true;
    }
    void Shutdown() override { m_Ready = false; }
    void Update(const SubsystemContext&) override {}
    void Render(rhi::IRHICommandList*) override {}
    void Bind(rhi::IRHICommandList*) const override {}
    void OnResize(u32, u32) override {}

    [[nodiscard]] const char* GetName()  const override { return "AA_None"; }
    [[nodiscard]] bool        IsReady()  const override { return m_Ready; }
    [[nodiscard]] bool        IsEnabled() const override { return false; }
    void SetEnabled(bool) override {}

    // ---- IPostProcessPass ----

    void SetInput(rhi::IRHITexture* tex, rhi::IRHISampler* samp) override {
        m_Input        = tex;
        m_InputSampler = samp;
    }
    [[nodiscard]] rhi::Format GetOutputFormat() const override {
        // 透传模式：输出格式等于输入格式（管线自行判断）
        return rhi::Format::RGBA16_FLOAT;
    }
    [[nodiscard]] rhi::IRHITexture* GetOutputTexture() const override { return m_Input; }
    [[nodiscard]] rhi::IRHISampler* GetOutputSampler() const override { return m_InputSampler; }

    // ---- IAntiAliasing ----

    [[nodiscard]] AAMode GetMode() const override { return AAMode::None; }

private:
    bool m_Ready = false;
    rhi::IRHITexture* m_Input        = nullptr;
    rhi::IRHISampler* m_InputSampler = nullptr;
};

} // namespace he::render
