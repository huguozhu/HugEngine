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

    /// 设置当前**飞行帧槽位**（`[0, rhi::kMaxFramesInFlight)`）；调用顺序：SetFrameSlot → SetInput → Render。
    /// 【为什么需要】HDR 输入按飞行帧轮换时（ForwardPipeline 的 HDR 目标每帧清空重画，必须分份），
    /// 输入描述符集也必须按槽位分开：单份集被每帧重写会在"帧仍在飞"时改写 GPU 正在读的描述符
    /// （未定义行为）。**不调用**则保持旧行为（输入指针不变就不重写），供 HDR 输入固定的管线使用。
    void SetFrameSlot(u32 slot) { m_Slot = slot % rhi::kMaxFramesInFlight; m_SlotAware = true; }

    /// ToneMap 输出为 LDR sRGB（BGRA8_UNORM，匹配交换链）
    [[nodiscard]] rhi::Format GetOutputFormat() const override { return m_OutputFormat; }

    // ---- ToneMap 特有 ----

    /// 设置自动曝光值
    void SetExposure(float e) { m_Exposure = e; }
    /// 设置输出格式（SDR=BGRA8_UNORM，HDR=A2B10G10R10_UNORM_PACK32），格式变化时重建 PSO
    void SetOutputFormat(rhi::Format f);
    /// 设置 HDR 开关（1=PQ 输出，0=sRGB 输出）
    void SetHDREnabled(bool h) { m_HDR = h ? 1.0f : 0.0f; }
    /// 设置 HDR 参考白点（尼特）
    void SetWhitePointNits(float w) { m_WhitePointNits = w; }
    /// 预设 PSO 为下一个 RenderPass 的初始管线
    // A-3：PSO 持句柄、按需解析（代次过期 ⇒ nullptr）
    void PreBind(rhi::IRHICommandList* cmd) {
        if (m_Ready && m_Device) cmd->SetPipeline(m_Device->ResolvePipelineState(m_PSO));
    }
    rhi::IRHIPipelineState* GetPSO() const {
        return m_Device ? m_Device->ResolvePipelineState(m_PSO) : nullptr;
    }

private:
    void RecreatePSO();

    rhi::ShaderBytecode m_VS, m_FS;
    rhi::RHIPipelineStateHandle m_PSO{};   // A-3：句柄持有
    rhi::DescriptorSetLayoutHandle m_DescLayout = rhi::kInvalidLayout;
    // 输入集按飞行帧各一份（见 SetFrameSlot）
    rhi::DescriptorSetHandle       m_DescSet[rhi::kMaxFramesInFlight] = {};
    rhi::IRHITexture*  m_HDRTarget[rhi::kMaxFramesInFlight] = {};   // 逐槽位输入缓存（槽位模式下按槽位比较）
    rhi::IRHISampler*  m_HDRSampler = nullptr;
    u32  m_Slot = 0;
    bool m_SlotAware = false;   // false = 旧行为（输入不变不重写）
    float m_Exposure = 1.0f;
    rhi::Format m_OutputFormat = rhi::Format::BGRA8_UNORM;
    float m_HDR = 0.0f;                 // 1=HDR10 PQ，0=SDR sRGB
    float m_WhitePointNits = 80.0f;     // HDR 参考白点（尼特）
    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false;
};

} // namespace he::render
