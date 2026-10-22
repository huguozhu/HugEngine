#pragma once

#include "Core/Types.h"

#include "RHI/RHI.h"   // T2.6：上下文成为渲染层调用 RHI 的唯一出口（本文件自此不再是 RHI-free）

#include <cstdint>

// ============================================================
// RenderThreadContext —— 渲染线程上下文（渲染线程化方案 B §4.3 / 阶段 0 T0.3）
//
// 游戏线程入队的每条命令在**渲染线程**上执行时拿到这个上下文。它的定位是"渲染线程的身份 +
// 帧信息"，并且（T2.6 起）是**渲染层调用 RHI 的唯一出口** —— 命令载荷里不允许出现裸
// `IRHIDevice*`，否则将来把执行位置换到 RHI 线程（§12 附录 C）时要重写整个渲染层。
//
// 【T2.6 的两段式】命令只负责"记录"（往 cmdList 里录），真正提交由上下文统一做：
//   - `Submit(cmd, stream)`：记录阶段结束后的**执行**入口（模式 2 下会走设备侧提交路由）；
//   - `CreateBufferHandle` / `CreateTextureHandle` / ...：**资源创建**也收口在这里，
//     将来把创建挪去 RHI 线程时只需改这一层，调用方拿到的句柄语义不变。
// ============================================================

namespace he::render {

struct RenderThreadContext {
    u64 frameIndex = 0;   // 单调递增的帧号（渲染线程视角）
    u32 frameSlot  = 0;   // 帧槽位（0 .. kMaxFramesInFlight-1），用于按帧轮换的资源

    /// 该帧是否被标记为"本帧有实际渲染工作"（空帧可跳过剔除等重活）
    bool hasWork = true;

    /// 【T2.6】渲染线程所属设备：由 `RenderThread` 在启动/泵帧时填入。
    /// 命令与 pass 一律通过本上下文访问设备，不要自己保存裸指针。
    rhi::IRHIDevice* device = nullptr;

    [[nodiscard]] bool IsValid() const { return device != nullptr; }

    // ---- 执行阶段（两段式的第二段）----
    /// 提交已录制好的命令列表；`stream` 非空时由设备按命令流回放（L2 管道）。
    /// 呈现不在这里：acquire/present 由 `rhi::FramePresenter`（L4-1）收口。
    void Submit(rhi::IRHICommandList* cmd, rhi::RHICommandList* stream = nullptr) {
        if (device && cmd) device->Submit(cmd, stream);
    }

    // ---- 资源创建（收口）----
    [[nodiscard]] rhi::RHIBufferHandle CreateBufferHandle(const rhi::BufferDesc& desc) {
        return device ? device->CreateBufferHandle(desc) : rhi::RHIBufferHandle{};
    }
    [[nodiscard]] rhi::RHITextureHandle CreateTextureHandle(const rhi::TextureDesc& desc) {
        return device ? device->CreateTextureHandle(desc) : rhi::RHITextureHandle{};
    }
    [[nodiscard]] rhi::RHIPipelineStateHandle CreatePipelineStateHandle(const rhi::PipelineStateDesc& desc) {
        return device ? device->CreatePipelineStateHandle(desc) : rhi::RHIPipelineStateHandle{};
    }
    [[nodiscard]] rhi::RHISamplerHandle CreateSamplerHandle(const rhi::SamplerDesc& desc) {
        return device ? device->CreateSamplerHandle(desc) : rhi::RHISamplerHandle{};
    }

    // ---- 解析（调用方只拿句柄，按需解析成后端对象）----
    [[nodiscard]] rhi::IRHIBuffer* ResolveBuffer(rhi::RHIBufferHandle h) const {
        return device ? device->ResolveBuffer(h) : nullptr;
    }
    [[nodiscard]] rhi::IRHITexture* ResolveTexture(rhi::RHITextureHandle h) const {
        return device ? device->ResolveTexture(h) : nullptr;
    }
    [[nodiscard]] rhi::IRHIPipelineState* ResolvePipelineState(rhi::RHIPipelineStateHandle h) const {
        return device ? device->ResolvePipelineState(h) : nullptr;
    }
    [[nodiscard]] rhi::IRHISampler* ResolveSampler(rhi::RHISamplerHandle h) const {
        return device ? device->ResolveSampler(h) : nullptr;
    }
};

} // namespace he::render
