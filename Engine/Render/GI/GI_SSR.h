#pragma once

#include "GI/GlobalIllumination.h"
#include "RHI/RHI.h"
#include "Math/Math.h"
#include <memory>

namespace he::render {

// ============================================================
// GI_SSR — 屏幕空间反射
//
// 每像素沿反射向量在深度缓冲中 Ray Marching，
// 命中的像素 albedo 作为间接镜面反射贡献。
// ============================================================
class GI_SSR : public IGlobalIllumination {
public:
    GI_SSR() = default;

    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) override;
    void Shutdown() override;
    void Update(const SubsystemContext&) override {}
    void Render(rhi::IRHICommandList* cmd) override;
    void Bind(rhi::IRHICommandList*) const override {}
    void OnResize(u32 w, u32 h) override;
    const char* GetName() const override { return "GI_SSR"; }
    bool IsReady() const override { return m_Ready; }
    bool IsEnabled() const override { return m_Settings.enabled; }
    void SetEnabled(bool e) override { m_Settings.enabled = e; }

    GIMode GetMode() const override { return GIMode::SSGI; }  // 暂用 SSGI mode
    rhi::IRHITexture* GetIndirectSpecularTexture() const override { return m_Output.get(); }

    void SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal, rhi::IRHITexture* albedo);
    /// 设置 Hi-Z 深度金字塔（层次追踪加速：大步长跳过低空区域，替代线性 march）
    void SetHiZ(rhi::IRHITexture* hiZ, rhi::IRHISampler* sampler);
    rhi::IRHISampler* GetOutputSampler() const { return m_Sampler.get(); }
    void PreBind(rhi::IRHICommandList* cmd) const { if (m_Ready) cmd->SetPipeline(m_PSO.get()); }

    float maxSteps   = 64;
    float stepSize   = 0.5f;
    float maxDistance = 50.0f;
    float thickness   = 0.1f;

private:
    void CreateOutputTex(u32 w, u32 h);
    // 半分辨率尺寸（GISettings.halfRes 时输出纹理降半——省约 3/4 像素着色）
    u32 halfResW(u32 w) const { return m_Settings.halfRes ? std::max(w / 2, 1u) : w; }
    u32 halfResH(u32 h) const { return m_Settings.halfRes ? std::max(h / 2, 1u) : h; }

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false;

    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_DescLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_DescSet    = rhi::kInvalidSet;

    std::unique_ptr<rhi::IRHITexture> m_Output;
    std::unique_ptr<rhi::IRHISampler> m_Sampler;
    std::unique_ptr<rhi::IRHISampler> m_PointSampler;

    /// 矩阵 UBO（binding 3）：invProj（clip→view）+ proj（view→clip）
    ///
    /// 为什么矩阵必须进 UBO：引擎保证的 push constant 范围只有 128B，而两个 mat4
    /// 恰好用满。此前只往 push constant 里传了**逆**矩阵，而 shader 还拿它当**正**投影
    /// 用（§9.2-B）——正投影无处安放正是该缺陷的成因。
    /// SSAO 早已采用「矩阵进 UBO」的同一做法（`SSAO.cpp` 的 projInv + proj）。
    std::unique_ptr<rhi::IRHIBuffer> m_UniformBuffer;

    rhi::IRHITexture* m_Depth  = nullptr;
    rhi::IRHITexture* m_Albedo = nullptr;
    rhi::IRHITexture* m_Normal = nullptr;
    rhi::IRHITexture* m_HiZTex = nullptr;      // Hi-Z 金字塔（不持有所有权）
    rhi::IRHISampler* m_HiZSampler = nullptr;  // Hi-Z 点采样器
};

} // namespace he::render
