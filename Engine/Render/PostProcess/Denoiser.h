#pragma once

#include "RHI/RHI.h"
#include "Math/Math.h"
#include <memory>

namespace he::render {

// ============================================================
// Denoiser — 5×5 双边模糊降噪（SSGI/SSR 共用）
//
// 两个权重（`depthSigma` / `normalSigma`）此前是着色器里的**固定常量**，4 个实例参数
// 完全相同 —— 也就是说同一个滤波核同时用在「间接漫反射」与「镜面反射」上，而这两种
// 信号的噪声分布与可容忍模糊度并不相同（§4.4 / 任务 11.1）。现在它们可配：
// 数值越大越"挑边"（越不容易跨过深度/法线不连续处，滤波越弱）。
// ============================================================
class Denoiser {
public:
    // 双边模糊默认 sigma 值（= 改造前的固定常量，保持既有行为）
    static constexpr float kDefaultDepthSigma  = 10.0f;
    static constexpr float kDefaultNormalSigma = 8.0f;

    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();
    void OnResize(u32 w, u32 h);

    /// 设置双边权重的敏感度（越大越挑边 = 滤波越弱）。按信号类型区分时调用。
    void SetDepthSigma(float s)  { m_DepthSigma  = s; }
    void SetNormalSigma(float s) { m_NormalSigma = s; }
    [[nodiscard]] float GetDepthSigma()  const { return m_DepthSigma; }
    [[nodiscard]] float GetNormalSigma() const { return m_NormalSigma; }

    void SetInputs(rhi::IRHITexture* color, rhi::IRHITexture* depth, rhi::IRHITexture* normal);
    void Render(rhi::IRHICommandList* cmd);

    rhi::IRHITexture* GetOutput() const { return m_Denoised.get(); }
    /// 当前工作尺寸。统一降噪框架（11.3）靠它判断"该级是否已经在信号分辨率上运行"——
    /// 半分辨率开关是运行时的，缓存一份尺寸副本必然会与纹理漂移，故直接问纹理。
    u32 GetWidth()  const { return m_Width; }
    u32 GetHeight() const { return m_Height; }
    bool IsReady() const { return m_Ready; }
    void PreBind(rhi::IRHICommandList* cmd) const { if(m_Ready) cmd->SetPipeline(m_PSO.get()); }

private:
    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width=0, m_Height=0; bool m_Ready=false;
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    std::unique_ptr<rhi::IRHITexture> m_Denoised;
    std::unique_ptr<rhi::IRHISampler> m_Sampler;
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;
    rhi::IRHITexture* m_Input=nullptr,*m_Depth=nullptr,*m_Normal=nullptr;
    float m_DepthSigma  = kDefaultDepthSigma;    // 见文件头：可配，默认保持既有行为
    float m_NormalSigma = kDefaultNormalSigma;
};

} // namespace he::render
