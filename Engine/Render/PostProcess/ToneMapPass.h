#pragma once

#include "PostProcess/PostProcessPass.h"
#include "RHI/RHI.h"
#include "RHI/Shader.h"
#include <memory>

namespace he::render {

// ============================================================================
// ToneMapPass — ACES ToneMap 后处理（全屏三角形，无需 VB/IB）
//
// 读取 HDR 离屏渲染目标 → ACES ToneMap → Linear→sRGB → 输出到当前 RenderPass。
// 继承 IPostProcessPass：不自拥有输出（写到外部 RP 目标）。
// ============================================================================
class ToneMapPass : public IPostProcessPass {
public:
    ToneMapPass()  = default;
    ~ToneMapPass() override = default;

    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) override;
    void Shutdown() override;
    void Update(const SubsystemContext&) override {}  // 无需场景数据
    void Render(rhi::IRHICommandList* cmd) override;
    void Bind(rhi::IRHICommandList* cmd) const override {}
    void OnResize(u32 width, u32 height) override;

    const char* GetName() const override { return "ToneMapPass"; }
    bool IsReady()   const override { return m_Ready; }
    bool IsEnabled() const override { return m_Enabled; }
    void SetEnabled(bool e) override { m_Enabled = e; }

    // ---- IPostProcessPass ----

    /// 每帧设置 HDR 输入纹理 + 采样器
    void SetInput(rhi::IRHITexture* hdrTarget, rhi::IRHISampler* sampler) override;

    /// ToneMap 输出为 LDR sRGB（BGRA8_UNORM，匹配交换链）
    [[nodiscard]] rhi::Format GetOutputFormat() const override { return rhi::Format::BGRA8_UNORM; }

    // ---- ToneMap 特有 ----

    /// 设置自动曝光值
    void SetExposure(float e) { m_Exposure = e; }
    /// 预设 PSO 为下一个 RenderPass 的初始管线
    void PreBind(rhi::IRHICommandList* cmd) { if (m_Ready) cmd->SetPipeline(m_PSO.get()); }
    rhi::IRHIPipelineState* GetPSO() const { return m_PSO.get(); }

private:
    rhi::ShaderBytecode m_VS, m_FS;
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_DescLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_DescSet    = rhi::kInvalidSet;
    rhi::IRHITexture*  m_HDRTarget  = nullptr;
    rhi::IRHISampler*  m_HDRSampler = nullptr;
    float m_Exposure = 1.0f;
    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false;
};

} // namespace he::render
