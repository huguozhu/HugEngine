#pragma once

// ============================================================
// GI/GIRadianceHistory.h — 前帧 HDR 辐射度（GI 源共享）
//
// 用途：把 Lighting 的输出下采样存一份，供**下一帧**的 GI 源当作真实入射辐射度采样。
// 这是「屏幕空间 GI 需要颜色输入」的公共前置：没有它，屏幕空间源只能拿 GBuffer 的
// 反照率凑数（§9.2-P），永远无法成为 E/π 的估计。
//
// 为什么取「前帧」：SSGI 在帧图里排在 Lighting **之前**（它要供 Lighting 采样），
// 同帧的 Lighting 结果此时尚不存在；取上一帧既解决了因果，又天然构成多次弹射的
// 时域反馈。DDGI 的探针更新同理。
//
// 为什么共享：DDGI 原本自己实现了一份（GI_DDGI 的 m_PrevHDR + CaptureHDR），
// 本组件即由那段代码抽出。GI 源越多，各自拷一份就等于每帧多付几次全屏下采样——
// 正是 §3.5 反对的「白付一份全量成本」。
//
// 尺寸：取源的 1 / kDownsampleFactor。GI 源在命中点处采样辐射度，不需要全分辨率。
// ============================================================

#include "RHI/RHI.h"
#include <memory>

namespace he::render {

class GIRadianceHistory {
public:
    /// 下采样倍率（源尺寸 / 该值）
    static constexpr u32 kDownsampleFactor = 4;

    /// 创建采样器、描述符集、下采样 PSO 与纹理。尺寸取源的 1/kDownsampleFactor。
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();

    /// 把 hdr（通常是 Lighting 的 HDR 目标）下采样到内部纹理。每帧在 Lighting 之后调用。
    /// 目标尺寸变化（窗口 resize）时自动重建纹理，并递增 generation。
    void Capture(rhi::IRHICommandList* cmd, rhi::IRHITexture* hdr);

    [[nodiscard]] rhi::IRHITexture* GetTexture() const { return m_Texture.get(); }
    [[nodiscard]] rhi::IRHISampler* GetSampler() const { return m_Sampler.get(); }
    [[nodiscard]] bool IsValid() const { return m_Texture != nullptr; }

    /// 纹理被（重新）创建过时递增。消费方把它缓存的代次与此比对，即可知道
    /// 是否需要重新绑定描述符——避免依赖「谁先 Initialize」这种脆弱假设。
    [[nodiscard]] u32 GetGeneration() const { return m_Generation; }

private:
    void CreateTexture(u32 width, u32 height, bool bumpGeneration);

    rhi::IRHIDevice* m_Device = nullptr;

    std::unique_ptr<rhi::IRHITexture>       m_Texture;
    std::unique_ptr<rhi::IRHISampler>       m_Sampler;
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;

    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;   // set=0：下采样输入

    u32 m_Generation  = 0;   // 纹理代次（见 GetGeneration）
    u32 m_SourceWidth = 0;   // 最近一次 Capture 的源尺寸，用于识别 resize
    u32 m_SourceHeight = 0;
};

} // namespace he::render
