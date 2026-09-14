#pragma once

// ============================================================
// GI/IBLProvider.h — IBL 环境源 Provider（Wave 2 推广）
//
// IBL 的特点：**没有独立 pass**——它的辐照度/预滤波贴图由天空盒烘焙而来
// （脏标记触发重建），产物被 Lighting 直接采样。因此：
//   · GetDiffuseOutput()  → 环境辐照度贴图（低频漫反射源）
//   · GetSpecularOutput() → 环境预滤波贴图（低频镜面源）
//   · Render()            → 仅在脏时重建（烘焙），无 GBuffer 依赖
//
// 它同时服务 diffuse 与 specular 两个通道（同一 Provider，两处各占一个层栈槽位）。
// ============================================================

#include "GI/IGIProvider.h"
#include "GI/GI_IBL.h"

namespace he::render {

/// IBL 环境源（低频：漫反射辐照度 + 镜面预滤波）
class IBLProvider final : public IGIProvider {
public:
    /// 注入底层的 IBL 实现（管线持有所有权）
    void SetPass(GI_IBL* ibl) { m_IBL = ibl; }

    [[nodiscard]] GISourceId GetSourceId() const override { return GISourceId::IBL; }
    [[nodiscard]] bool Handles(GISourceId id) const override { return id == GISourceId::IBL; }
    [[nodiscard]] bool IsValid() const override { return m_IBL != nullptr; }

    /// 低频环境源：属于 diffuse 与 specular 两个通道（层栈在两边各持一项）
    [[nodiscard]] bool NeedsPass(const GIChannelStack& stack) const override {
        return stack.Has(GISourceId::IBL) && IsValid();
    }

    [[nodiscard]] rhi::IRHITexture* GetDiffuseOutput() const override {
        return m_IBL ? m_IBL->GetIrradianceMap() : nullptr;   // 环境辐照度（漫反射）
    }
    [[nodiscard]] rhi::IRHITexture* GetSpecularOutput() const override {
        return m_IBL ? m_IBL->GetPrefilterMap() : nullptr;    // 环境预滤波（镜面）
    }

    bool Initialize(rhi::IRHIDevice*, u32, u32) override { return m_IBL != nullptr; }
    void Shutdown() override {}
    void OnResize(u32, u32) override {}
    /// IBL 产物由天空盒烘焙产生（脏时重建），无 GBuffer 依赖
    void Render(rhi::IRHICommandList* cmd, const GIProviderContext& /*ctx*/) override {
        if (m_IBL && m_IBL->IsDirty()) m_IBL->Render(cmd);
    }

    [[nodiscard]] GI_IBL* GetPass() const { return m_IBL; }

private:
    GI_IBL* m_IBL = nullptr;   // 非拥有
};

} // namespace he::render
