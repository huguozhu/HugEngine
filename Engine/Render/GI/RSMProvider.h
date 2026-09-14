#pragma once

// ============================================================
// GI/RSMProvider.h — RSM 间接光 Provider（Wave 2 推广）
//
// RSM（反射阴影贴图）从方向光视角渲染位置/通量图，再由 Lighting 的 shader
// 以 VPL（虚拟点光源）方式求和得到单次反弹间接光。特点：
//   · 需要场景数据（world / sceneGraph）→ 依赖 GIProviderContext
//   · 无独立的「通道输出纹理」：产物是位置图 + 通量图，被 shader 与 DDGI 共用
//   · 属于 diffuse 通道的中频源（与 SSGI 同频段，二者可选其一或并存）
// ============================================================

#include "GI/IGIProvider.h"
#include "GI/GI_RSM.h"

namespace he::render {

/// RSM 间接光源（中频：单次反弹 VPL）
class RSMProvider final : public IGIProvider {
public:
    /// 注入底层 RSM 实现（管线持有所有权）
    void SetPass(GI_RSM* rsm) { m_RSM = rsm; }

    [[nodiscard]] GISourceId GetSourceId() const override { return GISourceId::RSM; }
    [[nodiscard]] bool Handles(GISourceId id) const override { return id == GISourceId::RSM; }
    [[nodiscard]] bool IsValid() const override { return m_RSM != nullptr; }

    /// RSM 无独立通道输出（其产物由 shader 经 u_RSMPositionMap/u_RSMFluxMap 采样）
    [[nodiscard]] rhi::IRHITexture* GetDiffuseOutput() const override { return nullptr; }

    bool Initialize(rhi::IRHIDevice*, u32, u32) override { return m_RSM != nullptr; }
    void Shutdown() override {}
    void OnResize(u32, u32) override {}
    /// 生成 RSM（需要场景数据；由帧图在光源阴影之后、DDGI 之前调用）
    void Render(rhi::IRHICommandList* cmd, const GIProviderContext& ctx) override {
        if (m_RSM && ctx.world && ctx.sceneGraph) {
            m_RSM->RenderRSMPass(cmd, *ctx.world, *ctx.sceneGraph);
        }
    }

    [[nodiscard]] GI_RSM* GetPass() const { return m_RSM; }

private:
    GI_RSM* m_RSM = nullptr;   // 非拥有
};

} // namespace he::render
