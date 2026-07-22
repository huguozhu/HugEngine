#pragma once

#include "RHI/RHI.h"
#include "Math/Math.h"
#include <memory>
#include <vector>

// ============================================================
// SSAO — 屏幕空间环境光遮蔽
//
// 输入：深度 + 法线缓冲（GBuffer）
// 输出：单通道 AO 纹理（应用到环境光分量）
// 流程：SSAO Pass → Blur Pass → 应用到 Lighting
// ============================================================

namespace he::render {

class SSAO {
public:
    bool enabled = true;
    float radius      = 1.0f;   // 采样半径
    float bias        = 0.025f; // 深度偏移
    float intensity   = 1.0f;   // AO 强度
    int   sampleCount = 16;     // 每像素采样数

    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();
    void OnResize(u32 w, u32 h);

    /// 设置输入（GBuffer Depth + Normal）
    void SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal);

    /// 执行 SSAO + Blur Pass，写入 m_AOTexture
    void Render(rhi::IRHICommandList* cmd);

    // 输出
    rhi::IRHITexture* GetAOTexture() const { return m_AOTexture.get(); }
    rhi::IRHISampler* GetAOSampler() const { return m_AOSampler.get(); }
    void PreBind(rhi::IRHICommandList* cmd);  // 惰性创建 PSO 并绑定

private:
    void CreateAOTexture(u32 w, u32 h);
    void CreateBlurTexture(u32 w, u32 h);
    void GenerateKernel();
    void GenerateNoise(u32 size);

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false;

    // PSOs（惰性创建：首次 PreBind/Render 时由 CreatePipelineState 生成）
    std::unique_ptr<rhi::IRHIPipelineState> m_SSAO_PSO;
    std::unique_ptr<rhi::IRHIPipelineState> m_Blur_PSO;
    // PSO 描述符 + ShaderBytecode 副本（供惰性创建 + 预热队列使用）
    rhi::PipelineStateDesc m_SSAO_PsoDesc;
    rhi::PipelineStateDesc m_Blur_PsoDesc;
    rhi::ShaderBytecode    m_SSAO_VS, m_SSAO_FS;   // ShaderBytecode 副本（生命周期与 SSAO 对象一致）
    rhi::ShaderBytecode    m_Blur_VS,  m_Blur_FS;

    // 描述符集
    rhi::DescriptorSetLayoutHandle m_SSAOLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_SSAOSet    = rhi::kInvalidSet;
    rhi::DescriptorSetLayoutHandle m_BlurLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_BlurSet    = rhi::kInvalidSet;

    // 纹理
    std::unique_ptr<rhi::IRHITexture> m_AOTexture;    // AO 结果
    std::unique_ptr<rhi::IRHITexture> m_BlurTexture;  // 模糊中间纹理
    std::unique_ptr<rhi::IRHISampler> m_AOSampler;
    std::unique_ptr<rhi::IRHISampler> m_PointSampler; // 点采样（深度/法线）

    // 输入（不持有所有权）
    rhi::IRHITexture* m_DepthTex  = nullptr;
    rhi::IRHITexture* m_NormalTex = nullptr;

    // 随机采样内核 + 噪声
    static constexpr u32 kKernelSize = 64;  // SSAO 采样核大小
    std::vector<float4> m_Kernel;
    std::unique_ptr<rhi::IRHITexture> m_NoiseTex;

    // SSAO 参数 Uniform Buffer（对应 shader SSAOParams: kernel[64] + params + proj）
    std::unique_ptr<rhi::IRHIBuffer> m_ParamUBO;
};

} // namespace he::render
