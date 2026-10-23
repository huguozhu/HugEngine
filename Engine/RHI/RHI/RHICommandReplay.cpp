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
            if (d->instanceCount > 1u) {
                cmd.DrawIndexed(d->indexCount, d->instanceCount);   // 实例化重载（SV_InstanceID）
            } else {
                cmd.DrawIndexed(d->indexCount, d->startIndex, d->baseVertex);
            }
            ++replayed;
            break;
        }
        case RHICommandType::ExecuteSecondary: {
            const auto* d = stream.PayloadAt<RHICommandExecuteSecondary>(i);
            if (!d || !d->secondary) break;
            cmd.ExecuteSecondary(static_cast<IRHICommandList*>(d->secondary));
            ++replayed;
            break;
        }
        case RHICommandType::ExecuteIndirect:
            // 间接绘制路径尚未接入 A-1（需要先确认 `IRHICommandList` 的间接调用接口形态）
            break;
        case RHICommandType::SetDrawDebugLabel: {
            const auto* d = stream.PayloadAt<RHICommandSetDebugLabel>(i);
            if (!d) break;
            cmd.SetDrawDebugLabel(d->text);
            ++replayed;
            break;
        }
        case RHICommandType::SetViewport: {
            const auto* d = stream.PayloadAt<RHICommandSetViewport>(i);
            if (!d) break;
            Viewport vp{};
            vp.x = d->x; vp.y = d->y; vp.width = d->w; vp.height = d->h;
            vp.minDepth = d->minZ; vp.maxDepth = d->maxZ;
            cmd.SetViewport(vp);
            ++replayed;
            break;
        }
        case RHICommandType::SetScissor: {
            const auto* d = stream.PayloadAt<RHICommandSetScissor>(i);
            if (!d) break;
            ScissorRect sc{};
            sc.x = d->x; sc.y = d->y; sc.width = d->w; sc.height = d->h;
            cmd.SetScissor(sc);
            ++replayed;
            break;
        }
        case RHICommandType::CmdBeginRenderPass: {
            // L4-3: 命令流自带 render pass，先设置到目标列表（不依赖其历史状态）
            if (const auto* rp = stream.PayloadAt<RHICommandBeginRenderPass>(i); rp && rp->renderPass) {
                cmd.SetRenderPassHint(rp->renderPass);
            }
            const auto* d = stream.PayloadAt<RHICommandBeginRenderPass>(i);
            if (!d) break;
            const ClearValue* clear = nullptr;
            if (d->clearCount != 0u) {
                const auto* raw = stream.ArenaRawAt(d->clearOffset, sizeof(ClearValue) * d->clearCount);
                clear = reinterpret_cast<const ClearValue*>(raw);
            }
            cmd.BeginRenderPass(d->colorCount, static_cast<Format>(d->colorFormat),
                                static_cast<Format>(d->depthFormat), clear,
                                static_cast<LoadOp>(d->loadOp));
            ++replayed;
            break;
        }
        case RHICommandType::CmdEndRenderPass: {
            cmd.EndRenderPass();
            ++replayed;
            break;
        }
        case RHICommandType::CmdBeginOffscreenPass: {
            // L4-3: 命令流自带 render pass，先设置到目标列表（不依赖其历史状态）
            if (const auto* rp = stream.PayloadAt<RHICommandBeginOffscreenPass>(i); rp && rp->renderPass) {
                cmd.SetRenderPassHint(rp->renderPass);
            }
            const auto* d = stream.PayloadAt<RHICommandBeginOffscreenPass>(i);
            if (!d) break;
            const ClearValue* clear = nullptr;
            if (d->clearCount != 0u) {
                const auto* raw = stream.ArenaRawAt(d->clearOffset, sizeof(ClearValue) * d->clearCount);
                clear = reinterpret_cast<const ClearValue*>(raw);
            }
            cmd.BeginOffscreenPass(d->colorImageView, d->depthImageView, d->width, d->height,
                                   clear, d->allowSecondary != 0u);
            ++replayed;
            break;
        }
        case RHICommandType::CmdEndOffscreenPass: {
            cmd.EndOffscreenPass();
            ++replayed;
            break;
        }
        case RHICommandType::CmdDraw: {
            const auto* d = stream.PayloadAt<RHICommandDraw>(i);
            if (!d) break;
            cmd.Draw(d->vertexCount, d->instanceCount, d->firstVertex, d->firstInstance);
            ++replayed;
            break;
        }
        case RHICommandType::CmdPipelineBarrier: {
            const auto* d = stream.PayloadAt<RHICommandPipelineBarrier>(i);
            if (!d) break;
            cmd.PipelineBarrier(static_cast<PipelineStage>(d->srcStage),
                                static_cast<PipelineStage>(d->dstStage),
                                static_cast<ResourceState>(d->srcState),
                                static_cast<ResourceState>(d->dstState));
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
