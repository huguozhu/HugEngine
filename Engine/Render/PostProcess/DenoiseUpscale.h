#pragma once

// ============================================================
// PostProcess/DenoiseUpscale.h — 降噪后的「重建升采样」（统一降噪框架 11.3）
//
// 半分辨率信号的处理顺序是**先降噪、再重建升采样**：本类负责后一半，把
// `Denoiser` 在信号分辨率上的输出用全分辨率的深度/法线引导重建到消费端分辨率。
// 与 `Denoiser` 的关系是「同一条链的相邻两级」，故接口刻意同构
// （`PreBind` / `SetInputs` / `Render` / `GetOutput`），Provider 侧只需按索引串联。
//
// 【为什么不由 `Denoiser` 兼任】`Denoiser` 的职责是"在信号自己的分辨率上去噪"，
// 它的输出尺寸必须等于输入尺寸（核尺度 `texelSize` 由此确定）；把升采样塞进去就等于
// 让一个核同时承担两种尺度，半分辨率路径会退化成"最近邻复制 + 全分辨率核"。
// 拆开之后两级的尺度各自明确，也才有 `needsUpscale` 这个**可登记的信号属性**。
// ============================================================

#include "RHI/RHI.h"
#include "Math/Math.h"
#include <memory>

namespace he::render {

class DenoiseUpscale {
    HE_DECLARE_NON_COPYABLE(DenoiseUpscale);

public:
    DenoiseUpscale()  = default;
    ~DenoiseUpscale() = default;

    /// `outW/outH` = 消费端（全分辨率）尺寸
    bool Initialize(rhi::IRHIDevice* device, u32 outW, u32 outH);
    void Shutdown();
    void OnResize(u32 outW, u32 outH);

    /// 输入：信号分辨率的降噪结果 + 全分辨率引导。源尺寸每帧从纹理自身读取。
    void SetInputs(rhi::IRHITexture* color, rhi::IRHITexture* depth, rhi::IRHITexture* normal);

    /// 【必须在 BeginOffscreenPass 之前调用】绑定本 Pass 的管线（理由同 `Denoiser::PreBind`）
    void PreBind(rhi::IRHICommandList* cmd) const { if (m_Ready) cmd->SetPipeline(m_PSO.get()); }

    /// 执行重建升采样（调用方已 BeginOffscreenPass 之后调用）
    void Render(rhi::IRHICommandList* cmd);

    [[nodiscard]] rhi::IRHITexture* GetOutput() const { return m_Output.get(); }
    [[nodiscard]] u32 GetWidth()  const { return m_Width; }
    [[nodiscard]] u32 GetHeight() const { return m_Height; }
    [[nodiscard]] bool IsReady()  const { return m_Ready; }

    /// 引导参数（与降噪级保持一致；统一框架按信号赋值，见 11.1 的集中点）
    void SetDepthSigma(float s)  { m_DepthSigma  = s; }
    void SetNormalSigma(float s) { m_NormalSigma = s; }
    [[nodiscard]] float GetDepthSigma()  const { return m_DepthSigma; }
    [[nodiscard]] float GetNormalSigma() const { return m_NormalSigma; }

private:
    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false;

    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    std::unique_ptr<rhi::IRHITexture>       m_Output;
    // 线性采样：2× 升采样时 3×3 源邻域的采样点正好落在源纹素中心，线性=精确取值，
    // 同时对将来非整数倍率（1.5×/低分辨率探针）不会退化成块状复制
    std::unique_ptr<rhi::IRHISampler>       m_Sampler;
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;

    rhi::IRHITexture* m_Input  = nullptr;
    rhi::IRHITexture* m_Depth  = nullptr;
    rhi::IRHITexture* m_Normal = nullptr;

    float m_DepthSigma  = 10.0f;
    float m_NormalSigma = 8.0f;
};

} // namespace he::render
