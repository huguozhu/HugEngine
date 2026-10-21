#include "RHI/RHICommandReplay.h"

#include "RHI/RHICommandList.h"   // 记录端：RHICommand / RHICommandType / 载荷 POD
#include "RHI/CommandList.h"      // 执行端：IRHICommandList
#include "RHI/Buffer.h"           // IRHIBuffer
#include "RHI/Shader.h"           // IRHIPipelineState

namespace he::rhi {

u32 ReplayCommandStream(const RHICommandList& stream, IRHICommandList& cmd) {
    u32 replayed = 0;
    for (usize i = 0; i < stream.CommandCount(); ++i) {
        const RHICommand& c = stream.CommandAt(i);
        switch (c.type) {
        case RHICommandType::SetPipeline: {
            const auto* d = stream.PayloadAt<RHICommandSetPipeline>(i);
            if (!d || !d->pso) break;
            cmd.SetPipeline(static_cast<IRHIPipelineState*>(d->pso));
            ++replayed;
            break;
        }
        case RHICommandType::SetDescriptorSet: {
            const auto* d = stream.PayloadAt<RHICommandSetDescriptorSet>(i);
            if (!d) break;
            cmd.BindDescriptorSet(d->set, static_cast<DescriptorSetHandle>(d->handle));
            ++replayed;
            break;
        }
        case RHICommandType::SetPushConstants: {
            // 头在内联载荷、字节在 arena（shader 侧 offset 当前全部为 0，故只存大小与数据）
            const auto* d = stream.PayloadAt<RHICommandSetPushConstants>(i);
            if (!d) break;
            const u8* data = stream.ArenaRawAt(d->arenaOffset, d->size);
            if (!data) break;
            cmd.SetPushConstants(0u, d->size, data);
            ++replayed;
            break;
        }
        case RHICommandType::SetVertexBuffer: {
            const auto* d = stream.PayloadAt<RHICommandSetVertexBuffer>(i);
            if (!d || !d->buffer) break;
            cmd.SetVertexBuffer(static_cast<IRHIBuffer*>(d->buffer), d->slot);
            ++replayed;
            break;
        }
        case RHICommandType::SetIndexBuffer: {
            const auto* d = stream.PayloadAt<RHICommandSetIndexBuffer>(i);
            if (!d || !d->buffer) break;
            cmd.SetIndexBuffer(static_cast<IRHIBuffer*>(d->buffer));
            ++replayed;
            break;
        }
        case RHICommandType::DrawIndexed: {
            const auto* d = stream.PayloadAt<RHICommandDrawIndexed>(i);
            if (!d) break;
            cmd.DrawIndexed(d->indexCount, d->startIndex, d->baseVertex);
            ++replayed;
            break;
        }
        case RHICommandType::SetDrawDebugLabel: {
            const auto* d = stream.PayloadAt<RHICommandSetDebugLabel>(i);
            if (!d) break;
            cmd.SetDrawDebugLabel(d->text);
            ++replayed;
            break;
        }
        case RHICommandType::MarkSubmitBoundary:
            // 提交边界由执行端（RHI 线程）自己按帧处理，这里不是一条 RHI 调用
            ++replayed;
            break;
        default:
            // 未知类型 / 本阶段未接的类型（Dispatch/Barrier/CopyBuffer 等）：跳过并继续
            break;
        }
    }
    return replayed;
}

} // namespace he::rhi
