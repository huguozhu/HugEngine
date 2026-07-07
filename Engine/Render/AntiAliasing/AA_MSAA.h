// AntiAliasing/AA_MSAA.h — 硬件多重采样抗锯齿（Multi-Sample Anti-Aliasing）
//
// 通过覆盖 PSO 和 RT 描述符的 sampleCount 实现硬件 MSAA。
// 无需独立渲染 Pass — GPU 在光栅化阶段自动处理多重采样，
// 在 RenderPass 结束时自动 Resolve 到非多采样 Attachment。
//
// 管线兼容性：
//   Forward  ✅ — HDR RT 多采样，有效消除几何 + 着色锯齿
//   Deferred ⚠️ — GBuffer MRT 多采样代价过高，仅 HDR 目标多采样（减少着色锯齿）
//
#pragma once

#include "AntiAliasing.h"
#include "RHI/RHI.h"

namespace he::render {

class AA_MSAA final : public IAntiAliasing {
    HE_DECLARE_NON_COPYABLE(AA_MSAA);

public:
    AA_MSAA()  = default;
    ~AA_MSAA() override = default;

    // ── IRenderSubsystem ──
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) override {
        m_Device = device; m_Width = width; m_Height = height;
        m_Ready = true; return true;
    }
    void Shutdown() override { m_Device = nullptr; m_Ready = false; }
    void Update(const SubsystemContext&) override {}
    void Render(rhi::IRHICommandList*) override {}   // MSAA 无需独立 Pass
    void Bind(rhi::IRHICommandList*) const override {}
    void OnResize(u32 w, u32 h) override { m_Width = w; m_Height = h; }

    [[nodiscard]] const char* GetName()  const override { return "AA_MSAA"; }
    [[nodiscard]] bool        IsReady()  const override { return m_Ready; }
    [[nodiscard]] bool        IsEnabled() const override { return m_Enabled; }
    void SetEnabled(bool e) override { m_Enabled = e; }

    // ── IPostProcessPass ──  MSAA 不在后处理链中，这些方法无关
    void SetInput(rhi::IRHITexture*, rhi::IRHISampler*) override {}
    [[nodiscard]] rhi::Format GetOutputFormat() const override { return rhi::Format::RGBA8_UNORM; }
    [[nodiscard]] bool OwnsOutput() const override { return false; }

    // ── IAntiAliasing ──
    [[nodiscard]] AAMode GetMode() const override { return AAMode::MSAA; }

    // MSAA 专属：覆盖 RT/PSO 创建参数
    [[nodiscard]] bool RequiresMultisampling() const override {
        return m_Enabled && m_Ready;
    }
    [[nodiscard]] u32 GetSampleCount() const override {
        return m_SampleCount;
    }
    void SetSampleCount(u32 s) { m_SampleCount = s; }

    void OverrideTextureDesc(rhi::TextureDesc& d) const override {
        if (!m_Enabled || !m_Ready) return;
        d.sampleCount = m_SampleCount;
    }
    void OverridePSODesc(rhi::PipelineStateDesc& d) const override {
        if (!m_Enabled || !m_Ready) return;
        d.sampleCount = m_SampleCount;
    }

    // 管线兼容性
    [[nodiscard]] bool SupportsForward()  const override { return true; }
    [[nodiscard]] bool SupportsDeferred() const override { return true; }  // HDR 目标多采样

    // 配置
    u32 GetCurrentSampleCount() const { return m_SampleCount; }

private:
    rhi::IRHIDevice* m_Device  = nullptr;
    u32 m_Width  = 0;
    u32 m_Height = 0;
    bool m_Ready = false;
    bool m_Enabled = true;
    u32 m_SampleCount = 4;  // 默认 4x MSAA
};

} // namespace he::render
