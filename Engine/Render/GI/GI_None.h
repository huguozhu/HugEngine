#pragma once

#include "GI/GlobalIllumination.h"

namespace he::render {

// ============================================================================
// GI_None — 空 GI 实现（Null Object 模式）
//
// 所有方法为空操作或返回默认值。
// 作为"关闭 GI"的默认状态，调用者无需判空即可安全调用。
// ============================================================================
class GI_None : public IGlobalIllumination {
public:
    GI_None()  = default;
    ~GI_None() override = default;

    // ---- IRenderSubsystem 接口 ----
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) override;
    void Shutdown() override;
    void Update(const SubsystemContext& ctx) override;
    void Render(rhi::IRHICommandList* cmdList) override;
    void Bind(rhi::IRHICommandList* cmdList) const override;
    void OnResize(u32 width, u32 height) override;

    [[nodiscard]] const char* GetName()  const override { return "GI_None"; }
    [[nodiscard]] bool        IsReady()  const override { return true; } // 永不就绪 = 无计算
    [[nodiscard]] bool        IsEnabled() const override { return false; }
    void SetEnabled(bool enabled) override { (void)enabled; } // 空实现始终禁用

    // ---- IGlobalIllumination 接口 ----
    [[nodiscard]] GIMode GetMode() const override { return GIMode::None; }
};

} // namespace he::render
