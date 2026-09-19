// ============================================================
// PostProcess/DenoiseSignal.cpp — 统一降噪框架的实现（§10 的 11.3）
// ============================================================

#include "PostProcess/DenoiseSignal.h"

#include "Core/Log.h"

#include <cstring>
#include <functional>

namespace he::render {

// ============================================================
// DenoiseHistoryPool
// ============================================================
void DenoiseHistoryPool::Shutdown() {
    m_Entries.clear();
    m_Device = nullptr;
}

rhi::IRHITexture* DenoiseHistoryPool::Acquire(const char* name, u32 w, u32 h, rhi::Format fmt) {
    if (!m_Device || !name || w == 0 || h == 0) return nullptr;

    for (auto& e : m_Entries) {
        if (e.name == name && e.width == w && e.height == h && e.format == fmt) return e.tex.get();
    }

    rhi::TextureDesc d;
    d.format    = fmt;
    d.width     = w;
    d.height    = h;
    d.mipLevels = 1;
    // 与 RTDenoiser 原先自建历史完全一致：既是本帧的写入目标，又是下帧被采样的历史
    d.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;

    Entry e;
    e.name   = name;
    e.width  = w;
    e.height = h;
    e.format = fmt;
    e.tex    = m_Device->CreateTexture(d);
    if (!e.tex) {
        HE_CORE_WARN("DenoiseHistoryPool: 历史纹理创建失败（{}，{}x{}）", name, w, h);
        return nullptr;
    }
    HE_CORE_INFO("DenoiseHistoryPool: 新增历史纹理 {}（{}x{}，格式 {}）", name, w, h, (u32)fmt);
    m_Entries.push_back(std::move(e));
    return m_Entries.back().tex.get();
}

void DenoiseHistoryPool::OnResize(u32 /*w*/, u32 /*h*/) {
    // 尺寸变了：历史一律作废（内容与新尺寸无关），下一帧按新尺寸重建。
    // 调用方需把各自的"首帧"标记复位，否则会把未初始化的显存当成历史。
    if (!m_Entries.empty()) {
        HE_CORE_INFO("DenoiseHistoryPool: 视口变化，释放 {} 张历史纹理（下一帧按新尺寸重建）",
                     (u32)m_Entries.size());
    }
    m_Entries.clear();
}

u64 DenoiseHistoryPool::TotalBytes() const {
    u64 total = 0;
    for (const auto& e : m_Entries) {
        u32 bytesPerPixel = 8;   // 目前只有 RGBA16F 一类
        switch (e.format) {
            case rhi::Format::RGBA16_FLOAT: bytesPerPixel = 8; break;
            case rhi::Format::RGBA32_FLOAT: bytesPerPixel = 16; break;
            case rhi::Format::R16_FLOAT:    bytesPerPixel = 2; break;
            case rhi::Format::R32_FLOAT:    bytesPerPixel = 4; break;
            default:                        bytesPerPixel = 8; break;
        }
        total += (u64)e.width * e.height * bytesPerPixel;
    }
    return total;
}

void DenoiseHistoryPool::LogSummary(const char* tag) const {
    HE_CORE_INFO("降噪历史池[{}]（统一分配，{} 张，共 {} KB）", tag, (u32)m_Entries.size(),
                 TotalBytes() / 1024);
    for (const auto& e : m_Entries) {
        HE_CORE_INFO("   {:<28} {}x{}  格式 {}", e.name, e.width, e.height, (u32)e.format);
    }
}

// ============================================================
// DenoiseSignalRegistry（计数与契约说明在头里内联：单测无需链接渲染模块）
// ============================================================
void DenoiseSignalRegistry::LogSummary(const char* tag) {
    // 只在**信号名集合**变化时打印：集合稳定时每帧打印只是噪声，而"集合变了"恰恰是
    // 需要被看见的时刻（层栈切换、resize、新源接入）。
    size_t sig = 1469598103934665603ull;   // FNV-1a 起点
    for (const auto& s : m_Signals) {
        for (char ch : s.name) { sig ^= (size_t)(unsigned char)ch; sig *= 1099511628211ull; }
        sig ^= (size_t)(s.needsUpscale ? 1 : 2);
        sig *= 1099511628211ull;
    }
    if (sig == m_LastSignature) return;
    m_LastSignature = sig;

    HE_CORE_INFO("统一降噪框架[{}]（§10 的 11.3）: 当帧信号 {} 个 —— 有屏幕空间引导 {}、"
                 "需重建升采样 {}、有时域速度 {}",
                 tag, Count(), DenoiseCount(), UpscaleCount(), TemporalCount());
    for (const auto& s : m_Signals) {
        HE_CORE_INFO("   {:<24} {}x{} → {}x{}  {}{}{}  参数 depthSigma {:.1f} / normalSigma {:.1f}",
                     s.name, s.width, s.height, s.targetWidth, s.targetHeight,
                     s.HasSpatialGuide() ? "空间引导 " : "无空间引导 ",
                     (s.velocity ? "时域 " : "无时域 "),
                     (s.needsUpscale ? "需升采样" : ""),
                     (double)s.depthSigma, (double)s.normalSigma);
    }
    HE_CORE_INFO("   {}", denoise::ValidityContractName());
}

} // namespace he::render
