#pragma once

#include "PostProcess/PostProcessPass.h"
#include "Pipeline/Camera.h"
#include "Core/Types.h"

#include <glm/mat4x4.hpp>

namespace he::rhi {
    class IRHITexture;
    class IRHISampler;
    struct TextureDesc;
    struct PipelineStateDesc;
}

namespace he::render {

// ============================================================
// AAMode — 支持的 AA 技术枚举
// ============================================================
enum class AAMode : u8 {
    None = 0,   // 无 AA
    MSAA = 1,   // 硬件多重采样（仅前向渲染）
    TAA  = 2,   // 时域抗锯齿（仅延迟渲染，利用 GBuffer 深度/法线）
    FXAA = 3,   // 快速近似抗锯齿（前向 + 延迟均可）
    SMAA = 4,   // 未来：子像素形态学抗锯齿
};

// ============================================================
// AASettings — AA 配置参数
// ============================================================
struct AASettings {
    AAMode mode            = AAMode::None;
    u32    msaaSampleCount = 4;     // MSAA 采样数（2/4/8）
    bool   enabled         = true;
};

// ============================================================
// IAntiAliasing — 抗锯齿子系统接口
//
// 继承 IPostProcessPass（后处理链路接口），AA 特有方法在此扩展。
// SetInput/GetOutput/GetOutputFormat 等链路方法由基类 IPostProcessPass 提供。
//
//   ForwardPipeline（仅 MSAA / FXAA）:
//     Scene → ToneMap → [FXAA] → Present
//     ↑ MSAA 覆盖 RT 创建参数
//
//   DeferredPipeline（仅 TAA / FXAA）:
//     GBuffer → Lighting → [TAA] → ToneMap → [FXAA] → Present
//                       ↑ TAA 利用 GBuffer 深度/法线做邻域裁剪
// ============================================================
class IAntiAliasing : public IPostProcessPass {
    HE_DECLARE_NON_COPYABLE(IAntiAliasing);

public:
    IAntiAliasing() = default;
    ~IAntiAliasing() override = default;

    // ---- 模式查询 ----

    [[nodiscard]] virtual AAMode GetMode() const = 0;

    // ---- RT 创建参数覆盖（MSAA 专属） ----

    /// 是否需要在创建 RT/PSO 时覆盖采样数
    [[nodiscard]] virtual bool RequiresMultisampling() const { return false; }

    /// MSAA 采样数（RequiresMultisampling=true 时有效）
    [[nodiscard]] virtual u32 GetSampleCount() const { return 1; }

    /// 覆盖纹理创建描述符（MSAA 设置 sampleCount）
    virtual void OverrideTextureDesc(rhi::TextureDesc&) const {}

    /// 覆盖 PSO 创建描述符（MSAA 设置 sampleCount）
    virtual void OverridePSODesc(rhi::PipelineStateDesc&) const {}

    // ---- TAA 专属：抖动投影 ----

    /// 当前帧的子像素抖动偏移（NDC 空间 [-1, 1]）
    [[nodiscard]] virtual float2 GetJitterOffset() const { return {0.0f, 0.0f}; }

    /// 帧首调用：推进抖动序列、更新历史状态
    virtual void OnBeginFrame() {}

    // ---- 管线兼容性 ----

    /// MSAA=true（硬件采样覆盖 RT），TAA=false（需 GBuffer 深度/法线），FXAA=true
    [[nodiscard]] virtual bool SupportsForward()  const { return true; }

    /// MSAA=false（GBuffer MRT 多采样代价过高），TAA=true（复用 GBuffer），FXAA=true
    [[nodiscard]] virtual bool SupportsDeferred() const { return true; }

    // ---- 配置 ----

    [[nodiscard]] const AASettings& GetSettings() const { return m_Settings; }
    void SetSettings(const AASettings& s) { m_Settings = s; }

protected:
    AASettings m_Settings;
};

} // namespace he::render
