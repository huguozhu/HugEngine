#pragma once

// ============================================================
// GI/SpatialDenoiseAux.h — 「主输出 → 空间降噪 →（半分辨率时）重建升采样」附属链的共享实现
//
// 为什么需要它：SSGI 与 SSR 的降噪链在形状上是**同一条**（主 pass 输出 → 5×5 双边
// 滤波 → 供 Lighting 采样），此前两个 Provider 各写了一遍逐行同构的实现。复制粘贴的
// 代价不是行数，而是**漂移**：改一处忘了另一处，两个源的降噪行为就会悄悄分叉。
// 这里把「附属 pass 的数量/输入/输出/PreBind/Render」收敛成一份实现，Provider 只提供
// 三个变量（主输出、深度、法线）与一个 pass 名前缀。
//
// 与「统一降噪框架」的关系：这是三步里的第 11.1 步（去重 + 参数可配），不改变任何行为；
// 链条数据化（11.2）与按信号分派（11.3）另做。**这项工作（原 GI 计划任务 11）已迁到
// 《Lumen设计与实现》§10**（它的真正消费方是 Lumen 的多信号共存）；
// 设计与现状对照留在 GI 计划的 §4.4。本文件是现役实现，不因任务迁走而改变。
//
// 【步骤 34（11.3）新增第二级】半分辨率信号此前**根本不降噪**（`AuxActive()` 在
// `halfRes` 时返回 false，半分辨率输出被直接采样），而半分辨率恰恰最需要降噪。现在链条
// 按信号分辨率自适应：
//   · 信号 = 全分辨率：`[Denoise]`（与改造前逐字节相同）
//   · 信号 < 全分辨率：`[Denoise@信号分辨率] → [Upscale→全分辨率]`
// 两级的尺寸都由**纹理实况**推导（`SpatialDenoiseAux::SyncSizes` 每帧核对），因此运行时
// 切 `halfRes` 也能当场生效，不需要额外的重建事件。
// ============================================================

#include "PostProcess/Denoiser.h"
#include "PostProcess/DenoiseUpscale.h"

#include <cstdlib>
#include <cstring>

namespace he::render {

/// 半分辨率信号的降噪链形态（见 `SpatialDenoiseAux::Mode`）
enum class HalfResMode : u8 { Off, Denoise, Upscale };

/// Provider 侧的降噪附属链适配器（持有 Denoiser 的**非拥有**指针；升采样级为本类自有）
class SpatialDenoiseAux {
public:
    /// 升采样级需要设备来建纹理/管线；`fullW/fullH` 是消费端（全分辨率）尺寸
    void Initialize(rhi::IRHIDevice* device, u32 fullW, u32 fullH) {
        m_Upscale.Initialize(device, fullW, fullH);
    }
    void Shutdown() { m_Upscale.Shutdown(); }
    void OnResize(u32 fullW, u32 fullH) { m_Upscale.OnResize(fullW, fullH); }

    void SetPass(Denoiser* denoiser) { m_Denoise = denoiser; }
    [[nodiscard]] Denoiser* GetPass() const { return m_Denoise; }

    /// 每帧核对两级的分辨率（帧图构图之前调用，与 `SyncToStack` 同一时机）：
    /// 降噪级跟着**信号**分辨率走，升采样级跟着**消费端**分辨率走。
    /// 只在尺寸真的不同时才重建纹理（`OnResize` 每次都会建一张新纹理）。
    void SyncSizes(u32 signalW, u32 signalH, u32 fullW, u32 fullH) {
        if (signalW == 0u || signalH == 0u) return;
        if (m_Denoise && (m_Denoise->GetWidth() != signalW || m_Denoise->GetHeight() != signalH)) {
            m_Denoise->OnResize(signalW, signalH);
        }
        if (m_Upscale.IsReady() &&
            (m_Upscale.GetWidth() != fullW || m_Upscale.GetHeight() != fullH)) {
            m_Upscale.OnResize(fullW, fullH);
        }
        m_SignalW = signalW; m_SignalH = signalH;
        m_FullW   = fullW;   m_FullH   = fullH;
    }

    /// 是否需要重建升采样（信号分辨率低于消费端）
    /// 【验证开关】环境变量 `HE_DENOISE_HALFRES` 取 `off` / `denoise` / `full`（默认 full）：
    ///   · `off`     —— 半分辨率**既不降噪也不升采样**（= 步骤 34 之前的行为）
    ///   · `denoise` —— 半分辨率只降噪，结果被消费端**直接采样**（= 只做一半的中间态）
    ///   · `full`    —— 降噪 + 重建升采样（默认；11.3 的判据）
    /// 三种形态共用同一个可执行文件与同一份配置 ⇒ 三者的读数差就是「这一步到底买到了什么」，
    /// 而不是"两个不同构建之间的差异"（本轮之前的口径反复踩过这个坑）。
    [[nodiscard]] static HalfResMode Mode() {
        static const HalfResMode s_mode = []() {
            const char* v = std::getenv("HE_DENOISE_HALFRES");
            if (v && std::strcmp(v, "off") == 0)     return HalfResMode::Off;
            if (v && std::strcmp(v, "denoise") == 0) return HalfResMode::Denoise;
            return HalfResMode::Upscale;
        }();
        return s_mode;
    }
    /// 降噪链是否启用（`off` 时整条链都不注册，回到"半分辨率直接采样"）
    [[nodiscard]] bool Active() const {
        return m_Denoise != nullptr && Mode() != HalfResMode::Off;
    }
    [[nodiscard]] bool NeedsUpscale() const {
        if (Mode() != HalfResMode::Upscale) return false;
        return m_Upscale.IsReady() && m_SignalW != 0u && (m_SignalW < m_FullW || m_SignalH < m_FullH);
    }

    /// 附属 pass 数量：降噪 1 个（`wanted` 且降噪器存在）+ 需要升采样时再加 1 个
    [[nodiscard]] u32 Count(bool wanted) const {
        if (!wanted || !m_Denoise) return 0u;
        return NeedsUpscale() ? 2u : 1u;
    }
    /// 链尾输出（升采样级存在则取它，否则取降噪级）
    [[nodiscard]] rhi::IRHITexture* Output() const {
        if (NeedsUpscale()) return m_Upscale.GetOutput();
        return m_Denoise ? m_Denoise->GetOutput() : nullptr;
    }
    /// pass 名：两级各由 Provider 给名（第 0 级 = 降噪，第 1 级 = 升采样）
    [[nodiscard]] static const char* PassName(u32 i, const char* denoiseName,
                                             const char* upscaleName) {
        return (i == 1u) ? upscaleName : denoiseName;
    }
    /// 第 i 级的输入：0 = 主 pass 输出，1 = 降噪级输出
    [[nodiscard]] rhi::IRHITexture* PassInput(u32 i, rhi::IRHITexture* main) const {
        if (i == 0u) return main;
        return m_Denoise ? m_Denoise->GetOutput() : nullptr;
    }
    [[nodiscard]] rhi::IRHITexture* PassOutput(u32 i) const {
        if (i == 1u) return m_Upscale.GetOutput();
        return m_Denoise ? m_Denoise->GetOutput() : nullptr;
    }
    [[nodiscard]] u32 PassWidth(u32 i) const {
        if (i == 1u) return m_Upscale.GetWidth();
        return m_Denoise ? m_Denoise->GetWidth() : 0u;
    }
    [[nodiscard]] u32 PassHeight(u32 i) const {
        if (i == 1u) return m_Upscale.GetHeight();
        return m_Denoise ? m_Denoise->GetHeight() : 0u;
    }
    void PreBind(rhi::IRHICommandList* cmd, u32 i) const {
        if (i == 1u) { m_Upscale.PreBind(cmd); return; }
        if (m_Denoise) m_Denoise->PreBind(cmd);
    }
    /// 执行第 i 级：0 = 降噪（color = 主输出），1 = 重建升采样（color = 降噪输出）
    void Render(rhi::IRHICommandList* cmd, u32 i, rhi::IRHITexture* main,
                rhi::IRHITexture* depth, rhi::IRHITexture* normal) {
        if (i == 1u) {
            m_Upscale.SetInputs(m_Denoise ? m_Denoise->GetOutput() : nullptr, depth, normal);
            m_Upscale.Render(cmd);
            return;
        }
        if (!m_Denoise) return;
        m_Denoise->SetInputs(main, depth, normal);
        m_Denoise->Render(cmd);
    }

    /// 引导参数（按信号赋值，见 11.1 的集中点）：两级取同一组值
    void SetGuideSigmas(float depthSigma, float normalSigma) {
        if (m_Denoise) {
            m_Denoise->SetDepthSigma(depthSigma);
            m_Denoise->SetNormalSigma(normalSigma);
        }
        m_Upscale.SetDepthSigma(depthSigma);
        m_Upscale.SetNormalSigma(normalSigma);
    }
    [[nodiscard]] float GetDepthSigma() const {
        return m_Denoise ? m_Denoise->GetDepthSigma() : m_Upscale.GetDepthSigma();
    }
    [[nodiscard]] float GetNormalSigma() const {
        return m_Denoise ? m_Denoise->GetNormalSigma() : m_Upscale.GetNormalSigma();
    }

private:
    Denoiser*      m_Denoise = nullptr;   // 非拥有；生命周期由管线管理
    DenoiseUpscale m_Upscale;             // 自有：半分辨率信号的重建升采样级
    u32 m_SignalW = 0, m_SignalH = 0;     // 信号分辨率（每帧由 SyncSizes 更新）
    u32 m_FullW   = 0, m_FullH   = 0;     // 消费端分辨率
};

} // namespace he::render
