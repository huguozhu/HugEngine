#pragma once

#include "Subsystem/RenderSubsystem.h"
#include "RHI/Types.h"  // rhi::Format 枚举

namespace he::rhi {
    class IRHITexture;
    class IRHISampler;
    class IRHIDevice;
    class IRHICommandList;
}

namespace he::render {

// ============================================================
// IPostProcessPass — 后处理 Pass 中间层
//
// 在 IRenderSubsystem 基础上增加统一的输入/输出链路接口。
// 所有后处理类 Pass（ToneMap、Bloom、DOF、MotionBlur 等）
// 以及后处理类 AA（TAA、FXAA）都继承此接口。
//
// 后处理链：
//   HDR → [TAA] → ToneMap → [FXAA] → BackBuffer
//   每个 Pass 通过 SetInput() 接收上游输出，
//   通过 GetOutputTexture() 暴露自身输出给下游。
//
// SkyboxPass 不继承此类 — 它是场景 Pass，不是后处理链一部分。
// ============================================================
class IPostProcessPass : public IRenderSubsystem {
public:
    IPostProcessPass() = default;
    ~IPostProcessPass() override = default;

    // ---- 输入（统一链路接口） ----

    /// 设置上游纹理输入 + 采样器（管线在 Pass 执行前调用）
    virtual void SetInput(rhi::IRHITexture* color, rhi::IRHISampler* sampler) = 0;

    // ---- 输出格式 ----

    /// 此 Pass 的输出格式（管线用于创建中间 RenderTarget）
    /// ToneMap/FXAA → BGRA8_UNORM; TAA → RGBA16_FLOAT
    [[nodiscard]] virtual rhi::Format GetOutputFormat() const = 0;

    // ---- 自拥有输出（TAA 等需要内部历史缓冲的 Pass） ----

    /// 是否自拥有输出纹理（true=Pass 内部管理输出目标）
    [[nodiscard]] virtual bool OwnsOutput() const { return false; }

    /// 自拥有时的输出纹理（OwnsOutput=true 时有效）
    [[nodiscard]] virtual rhi::IRHITexture* GetOutputTexture() const { return nullptr; }
    [[nodiscard]] virtual rhi::IRHISampler* GetOutputSampler() const { return nullptr; }
};

} // namespace he::render
