#pragma once

// ============================================================
// GI/SpatialDenoiseAux.h — 「主输出 → 空间降噪」附属 pass 的共享实现
//
// 为什么需要它：SSGI 与 SSR 的降噪链在形状上是**同一条**（主 pass 输出 → 5×5 双边
// 滤波 → 供 Lighting 采样），此前两个 Provider 各写了一遍逐行同构的实现。复制粘贴的
// 代价不是行数，而是**漂移**：改一处忘了另一处，两个源的降噪行为就会悄悄分叉。
// 这里把「附属 pass 的数量/输入/输出/PreBind/Render」收敛成一份实现，Provider 只提供
// 三个变量（主输出、深度、法线）与一个 pass 名。
//
// 与「统一降噪框架」的关系：这是三步里的第 11.1 步（去重 + 参数可配），不改变任何行为；
// 链条数据化（11.2）与按信号分派（11.3）另做。**这项工作（原 GI 计划任务 11）已迁到
// 《Lumen设计与实现》§10**（它的真正消费方是 Lumen 的多信号共存）；
// 设计与现状对照留在 GI 计划的 §4.4。本文件是现役实现，不因任务迁走而改变。
// ============================================================

#include "PostProcess/Denoiser.h"

namespace he::render {

/// Provider 侧的降噪附属 pass 适配器（持有 Denoiser 的**非拥有**指针）
class SpatialDenoiseAux {
public:
    void SetPass(Denoiser* denoiser) { m_Denoise = denoiser; }
    [[nodiscard]] Denoiser* GetPass() const { return m_Denoise; }

    /// 附属 pass 数量：只有该信号需要降噪（`wanted`）且 Denoiser 存在时才有一个
    [[nodiscard]] u32 Count(bool wanted) const { return (wanted && m_Denoise) ? 1u : 0u; }
    [[nodiscard]] rhi::IRHITexture* Output() const {
        return m_Denoise ? m_Denoise->GetOutput() : nullptr;
    }
    void PreBind(rhi::IRHICommandList* cmd) const {
        if (m_Denoise) m_Denoise->PreBind(cmd);
    }
    /// 执行降噪：color 是主 pass 输出，depth/normal 是 GBuffer 引导
    void Render(rhi::IRHICommandList* cmd, rhi::IRHITexture* color,
                rhi::IRHITexture* depth, rhi::IRHITexture* normal) {
        if (!m_Denoise) return;
        m_Denoise->SetInputs(color, depth, normal);
        m_Denoise->Render(cmd);
    }

private:
    Denoiser* m_Denoise = nullptr;   // 非拥有；生命周期由管线管理
};

} // namespace he::render
