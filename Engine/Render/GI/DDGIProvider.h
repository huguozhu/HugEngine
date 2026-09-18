#pragma once

// ============================================================
// GI/DDGIProvider.h — DDGI 动态漫反射探针 Provider（Wave 2 阶段 4）
//
// DDGI 与屏幕空间类的形态不同，是抽象的一次重要检验：
//   · pass 类型为 Compute（探针射线步进 + SH 投影）
//   · **无通道纹理输出**：产物是探针缓冲（SH 系数），由 Lighting 的 shader
//     通过 SampleDDGI() 直接采样，因此 HasTextureOutput() 返回 false
//   · 需要相机上下文（SubsystemContext）与 GBuffer 输入
//
// 帧图对这类源只需「按注册表执行 pass」，不做 ImportTexture 与输出依赖。
// ============================================================

#include "GI/IGIProvider.h"
#include "GI/GI_DDGI.h"

namespace he::render {

/// DDGI 探针源（低频，世界空间）
class DDGIProvider final : public IGIProvider {
public:
    /// 注入底层实现（管线持有所有权）
    void SetPass(GI_DDGI* ddgi) { m_DDGI = ddgi; }

    /// 由帧图注入相机上下文（探针捕获需要相机视锥/矩阵）
    void SetCamera(const CameraData* camera) { m_Camera = camera; }

    [[nodiscard]] GISourceId GetSourceId() const override { return GISourceId::DDGI; }
    [[nodiscard]] bool Handles(GISourceId id) const override { return id == GISourceId::DDGI; }
    [[nodiscard]] bool IsValid() const override { return m_DDGI && m_DDGI->IsEnabled(); }

    /// 同步到层栈：层栈是唯一真值（不变量 1）。
    /// DDGI 的开关此前只在管线 Initialize 时按层栈算一次，之后层栈再变（面板/配置/预设）
    /// 就与子系统脱节 —— 层栈里有 DDGI 而开关仍是关的，pass 不注册却照常参与归一化（§9.2-G）。
    void SyncToStack(const GIChannelStack& stack) override {
        if (m_DDGI) m_DDGI->SetEnabled(stack.Has(GISourceId::DDGI));
    }

    /// 探针更新是计算着色器 pass
    [[nodiscard]] GIPassKind GetPassKind() const override { return GIPassKind::Compute; }
    /// DDGI 探针的辐射度回退来源就是「前帧 HDR 辐射度」共享组件（见 GI_DDGI::SetIBL 的注释：
    /// 探头更新会采样它）→ 捕获门控必须把它算进消费者。
    [[nodiscard]] bool NeedsRadianceHistory() const override {
        return m_DDGI != nullptr && m_DDGI->IsEnabled();
    }
    /// 无通道纹理：产物是探针缓冲，由 shader 的 SampleDDGI() 直接读取
    [[nodiscard]] bool HasTextureOutput() const override { return false; }
    [[nodiscard]] rhi::IRHITexture* GetDiffuseOutput() const override { return nullptr; }

    void SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal,
                   rhi::IRHITexture* albedo) override {
        m_Depth = depth; m_Normal = normal; m_Albedo = albedo;
    }

    bool Initialize(rhi::IRHIDevice*, u32, u32) override { return m_DDGI != nullptr; }
    void Shutdown() override {}
    void OnResize(u32, u32) override {}

    /// 探针更新：先 Update（CPU 侧准备 + 参数），再 Render（提交计算 pass）
    void Render(rhi::IRHICommandList* cmd, const GIProviderContext& /*ctx*/) override {
        if (!m_DDGI) return;
        m_DDGI->SetGBufferInputs(m_Depth, m_Normal, m_Albedo);
        SubsystemContext dgiCtx;
        dgiCtx.camera = m_Camera;
        m_DDGI->Update(dgiCtx);
        m_DDGI->Render(cmd);
    }

    [[nodiscard]] GI_DDGI* GetPass() const { return m_DDGI; }
    [[nodiscard]] const CameraData* GetCamera() const { return m_Camera; }

private:
    GI_DDGI* m_DDGI = nullptr;            // 非拥有
    const CameraData* m_Camera = nullptr; // 非拥有（帧图每帧注入）
    rhi::IRHITexture* m_Depth  = nullptr;
    rhi::IRHITexture* m_Normal = nullptr;
    rhi::IRHITexture* m_Albedo = nullptr;
};

} // namespace he::render
