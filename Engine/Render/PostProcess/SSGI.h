#pragma once

#include "RHI/RHI.h"
#include "Math/Math.h"
#include <memory>
#include <vector>

namespace he::render {

// ============================================================
// SSGI — 屏幕空间全局光照（间接漫反射）
//
// 基于 SSAO 架构，额外采样 GBuffer albedo 作为间接光源。
// 每像素在半球内取 N 个采样点 → 深度测试 → 累加 Albedo。
// 输入：depth + normal + albedo（GBuffer）
// 输出：间接光照纹理（RGB16F）
// ============================================================
class SSGI {
public:
    bool enabled = true;
    float radius     = 1.0f;
    float intensity  = 1.0f;
    int   sampleCount = 16;

    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();
    void OnResize(u32 w, u32 h);

    void SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal, rhi::IRHITexture* albedo);
    void Render(rhi::IRHICommandList* cmd);

    rhi::IRHITexture* GetOutputTexture() const { return m_Output.get(); }
    rhi::IRHISampler* GetOutputSampler() const { return m_Sampler.get(); }

private:
    void CreateOutputTex(u32 w, u32 h);

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false;

    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_DescLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_DescSet    = rhi::kInvalidSet;

    std::unique_ptr<rhi::IRHITexture> m_Output;
    std::unique_ptr<rhi::IRHISampler> m_Sampler;
    std::unique_ptr<rhi::IRHISampler> m_PointSampler;

    rhi::IRHITexture* m_Depth   = nullptr;
    rhi::IRHITexture* m_Albedo  = nullptr;
    rhi::IRHITexture* m_Normal  = nullptr;
};

} // namespace he::render
