// AntiAliasing/AA_SMAA.h — 子像素形态学抗锯齿（Subpixel Morphological Anti-Aliasing）
//
// 3 Pass LDR 后处理抗锯齿，与 FXAA 互斥（二选一）：
//   Pass 1: EdgeDetection → 边缘纹理（R8，离屏）
//   Pass 2: BlendWeight → 混合权重（RGBA8，离屏）
//   Pass 3: Neighborhood → BackBuffer（终端，主渲染通道内）
//
// 管线位置：ToneMap → [ColorGrading] → [SMAA | FXAA] → BackBuffer
//
#pragma once

#include "AntiAliasing.h"
#include "RHI/RHI.h"
#include <memory>

namespace he::render {

class AA_SMAA final : public IAntiAliasing {
    HE_DECLARE_NON_COPYABLE(AA_SMAA);

public:
    AA_SMAA()  = default;
    ~AA_SMAA() override = default;

    // ── IRenderSubsystem ──
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) override;
    void Shutdown() override;
    void Update(const SubsystemContext&) override {}
    void Render(rhi::IRHICommandList* cmd) override;       // Pass 1+2（离屏）
    void Bind(rhi::IRHICommandList* cmd) const override {}
    void OnResize(u32 width, u32 height) override;

    [[nodiscard]] const char* GetName()  const override { return "AA_SMAA"; }
    [[nodiscard]] bool        IsReady()  const override { return m_Ready; }
    [[nodiscard]] bool        IsEnabled() const override { return m_Enabled; }
    void SetEnabled(bool e) override { m_Enabled = e; }

    // ── IPostProcessPass ──
    void SetInput(rhi::IRHITexture* color, rhi::IRHISampler* sampler) override;
    [[nodiscard]] rhi::Format GetOutputFormat() const override { return rhi::Format::BGRA8_UNORM; }
    [[nodiscard]] bool OwnsOutput() const override { return false; }  // 终端 Pass，写 BackBuffer
    [[nodiscard]] rhi::IRHITexture* GetOutputTexture() const override { return nullptr; }
    [[nodiscard]] rhi::IRHISampler* GetOutputSampler() const override { return nullptr; }

    // ── IAntiAliasing ──
    [[nodiscard]] AAMode GetMode() const override { return AAMode::SMAA; }

    // ── SMAA 专属 ──
    // Pass 3（Neighborhood Blending）：在 BeginRenderPass(BackBuffer) 内部调用
    // 调用者需确保已调用 SetInput + Render（Pass 1+2）
    void RenderFinalPass(rhi::IRHICommandList* cmd);

private:
    void CreateEdgePSO();
    void CreateBlendPSO();
    void CreateNeighborPSO();
    void DestroyAllPSOs();

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width  = 0;
    u32 m_Height = 0;
    bool m_Ready = false;
    bool m_Enabled = true;

    // 中间纹理（Pass 1/2 输出，分辨率相关，OnResize 重建）
    std::unique_ptr<rhi::IRHITexture> m_EdgeTex;   // R8_UNORM — 边缘类型编码
    std::unique_ptr<rhi::IRHITexture> m_BlendTex;  // RGBA8_UNORM — 四方向混合权重

    // 采样器
    std::unique_ptr<rhi::IRHISampler> m_PointSampler;   // Nearest-neighbor（边缘/权重纹理）
    std::unique_ptr<rhi::IRHISampler> m_LinearSampler;  // Linear（输入颜色）

    // Pass 1: Edge Detection
    std::unique_ptr<rhi::IRHIPipelineState> m_EdgePSO;
    rhi::DescriptorSetLayoutHandle m_EdgeLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_EdgeSet    = rhi::kInvalidSet;

    // Pass 2: Blending Weight Calculation
    std::unique_ptr<rhi::IRHIPipelineState> m_BlendPSO;
    rhi::DescriptorSetLayoutHandle m_BlendLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_BlendSet    = rhi::kInvalidSet;

    // Pass 3: Neighborhood Blending
    std::unique_ptr<rhi::IRHIPipelineState> m_NeighborPSO;
    rhi::DescriptorSetLayoutHandle m_NeighborLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_NeighborSet    = rhi::kInvalidSet;

    // 上游输入（由 SetInput 设置，外部管理生命周期）
    rhi::IRHITexture* m_Input        = nullptr;
    rhi::IRHISampler* m_InputSampler = nullptr;
};

} // namespace he::render
