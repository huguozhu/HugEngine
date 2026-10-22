// 像素数值探针的实现（机制）：读回缓冲 + timeline 等待 + half 解码 + 亮度。

#include "Debug/PixelProbe.h"

#include "Core/Log.h"

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>   // glm::unpackHalf1x16

namespace he::render {

namespace {
// RGBA16_FLOAT：每像素 8 字节（两个 half × RGBA）
constexpr usize kBytesPerPixel = 8;
} // namespace

bool PixelProbe::Initialize(rhi::IRHIDevice* device, u32 maxSamples) {
    if (!device || maxSamples == 0u) return false;

    rhi::BufferDesc bd;
    bd.size      = (usize)maxSamples * kBytesPerPixel;
    bd.usage     = rhi::BufferUsage::Storage;   // 该路径恒定带 TRANSFER_DST，可作拷贝目标
    bd.cpuAccess = true;                        // 需要 Map 读回
    m_Readback = device->CreateBuffer(bd);
    if (!m_Readback) {
        HE_CORE_ERROR("[像素探针] 读回缓冲创建失败（{} 个采样点）", maxSamples);
        return false;
    }
    m_Count  = maxSamples;
    m_Ready  = true;
    m_Values.assign(maxSamples, Value{});
    return true;
}

void PixelProbe::RecordReadback(rhi::IRHICommandList* cmd, rhi::IRHITexture* tex,
                                const Sample* samples, u32 count) {
    if (!m_Ready || !cmd || !tex || !samples) return;
    if (count > m_Count) count = m_Count;

    for (u32 i = 0; i < count; ++i) {
        // 每个采样点：1×1 像素，落到缓冲里第 i 个 8 字节槽位
        cmd->CopyTextureToBuffer(tex, m_Readback.get(), samples[i].x, samples[i].y, 1, 1,
                                 (usize)i * kBytesPerPixel);
    }
    m_Recording = true;
}

void PixelProbe::FinishRecording(rhi::IRHIDevice* device, rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_Recording || !device || !cmd) return;

    // 给本帧挂 timeline 信号：读取侧只等这一次提交（不做设备级 WaitIdle）
    if (!m_Fence) m_Fence = std::make_unique<CrossThreadFence>();
    if (m_Fence->fence.load() == rhi::kInvalidFence) {
        m_Fence->fence.store(device->CreateFence());   // 拥有线程（渲染线程）上创建
    }
    const u64 fv = m_Fence->value.load() + 1ull;
    m_Fence->value.store(fv);
    cmd->SetTimelineSignal(m_Fence->fence.load(), fv);
}

bool PixelProbe::TryReadBack(rhi::IRHIDevice* device) {
    if (!m_Ready || !m_Recording || !device || !m_Readback) return false;

    // 模式 2 下已录制但还没提交的工作在 RHI 线程队列里 ⇒ 先让它排空，再等本帧时间线值
    if (auto* router = device->GetSubmitRouter(); router != nullptr) router->FlushAndWait();
    if (m_Fence && m_Fence->fence.load() != rhi::kInvalidFence) {
        device->WaitForFence(m_Fence->fence.load(), m_Fence->value.load(), UINT64_MAX);
    } else {
        device->WaitIdle();   // 兜底（理论上不会走到：FinishRecording 时就建了）
    }

    const auto* texels = static_cast<const uint16_t*>(m_Readback->Map());
    if (!texels) {
        HE_CORE_ERROR("[像素探针] 映射失败");
        return false;
    }
    for (u32 i = 0; i < m_Count; ++i) {
        // 每像素 8 个 half：RGBA 各 2 字节（第 4 个分量未使用）
        const uint16_t* px = texels + (usize)i * 4;
        Value& v = m_Values[i];
        for (int c = 0; c < 3; ++c) v.rgb[c] = glm::unpackHalf1x16(px[c]);
        // Rec.709 亮度（与历史判据一致：0.2126 / 0.7152 / 0.0722）
        v.luminance = 0.2126f * v.rgb[0] + 0.7152f * v.rgb[1] + 0.0722f * v.rgb[2];
    }
    m_Readback->Unmap();
    m_Recording = false;
    return true;
}

float PixelProbe::LuminanceRatio(u32 a, u32 b) const {
    if (a >= m_Values.size() || b >= m_Values.size()) return 0.0f;
    const float denom = m_Values[b].luminance;
    return (denom > 1e-6f) ? (m_Values[a].luminance / denom) : 0.0f;
}

} // namespace he::render
