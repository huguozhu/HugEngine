#pragma once

#include "Shadow/IShadowSystem.h"

namespace he::render {

// ============================================================================
// ShadowNone — 空阴影子系统（Null Object 模式）
//
// 所有方法为空操作，用于阴影关闭时的默认实现。
// 避免 ForwardPipeline 中的 nullptr 检查。
// ============================================================================
class ShadowNone : public IShadowSystem {
public:
    ShadowNone()  = default;
    ~ShadowNone() override = default;

    // IShadowSystem
    Mode GetMode() const override { return Mode::None; }
    void NextFrame() override {}
    void SetRenderResources(rhi::IRHIBuffer*, rhi::IRHIBuffer*, rhi::DescriptorSetHandle) override {}

    u32 GetShadowMapCount() const override { return 0; }
    rhi::IRHITexture* GetShadowMap(u32) const override { return nullptr; }
    rhi::IRHISampler* GetShadowSampler() const override { return nullptr; }

    i32 GetShadowIndex(Entity) const override { return -1; }
    void CreateShadowPSO(rhi::DescriptorSetLayoutHandle) override {}
    bool HasActiveShadows() const override { return false; }

    // IRenderSubsystem（空实现）
    bool Initialize(rhi::IRHIDevice*, u32, u32) override { m_Ready = true; return true; }
    void Shutdown() override { m_Ready = false; }
    void Update(const SubsystemContext&) override {}
    void Render(rhi::IRHICommandList*) override {}
    void Bind(rhi::IRHICommandList*) const override {}
    void OnResize(u32, u32) override {}

    const char* GetName()  const override { return "ShadowNone"; }
    bool        IsReady()  const override { return m_Ready; }
    bool        IsEnabled() const override { return false; }
    void        SetEnabled(bool) override {}

private:
    bool m_Ready = false;
};

} // namespace he::render
