#include "RHI/RHICommandReplay.h"
#include "Core/Log.h"

#include "RHI/RHICommandList.h"   // 记录端：RHICommand / RHICommandType / 载荷 POD
#include "RHI/CommandList.h"      // 执行端：IRHICommandList
#include "RHI/Buffer.h"           // IRHIBuffer
#include "RHI/Shader.h"           // IRHIPipelineState
#include "RHI/QueryPool.h"        // IRHIQueryPool（查询类命令的回放）
#include "RHI/RayTracing.h"       // 【P3】BLAS 构建 / TraceRays 的 RHI 描述结构

namespace he::rhi {

static u32 g_L43PassDraws = 0;
static u32 g_L43PassW = 0, g_L43PassH = 0;

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
            cmd.SetPushConstants(d->offset, d->size, data);
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
            cmd.SetIndexBuffer(static_cast<IRHIBuffer*>(d->buffer), d->offset);
            ++replayed;
            break;
        }
        case RHICommandType::DrawIndexed: {
            const auto* d = stream.PayloadAt<RHICommandDrawIndexed>(i);
            if (!d) break;
            // L4-3 修正：旧实现把实例化重载当成"两参数"，丢掉了 startIndex/baseVertex，
            // 且把 instanceCount 传到了 firstIndex 的位置 ⇒ 实例化绘制会画错/画空。
            // 统一用五参数重载，语义与录制侧一一对应。
            cmd.DrawIndexed(d->indexCount, d->instanceCount, d->startIndex, d->baseVertex, d->firstInstance);
            ++g_L43PassDraws;
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
            g_L43PassDraws = 0; g_L43PassW = d->width; g_L43PassH = d->height;

            ++replayed;
            break;
        }
        case RHICommandType::CmdEndOffscreenPass: {
            if (std::getenv("HE_L43_PASSDRAW")) {
                HE_CORE_INFO("[L4-3] passdraws offscreen {}x{} draws={}", g_L43PassW, g_L43PassH, g_L43PassDraws);
            }
            cmd.EndOffscreenPass();
            ++replayed;
            break;
        }
        case RHICommandType::CmdDraw: {
            const auto* d = stream.PayloadAt<RHICommandDraw>(i);
            if (!d) break;
            cmd.Draw(d->vertexCount, d->instanceCount, d->firstVertex, d->firstInstance);
            ++g_L43PassDraws;
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
        case RHICommandType::CmdDispatch: {
            const auto* d = stream.PayloadAt<RHICommandDispatch>(i);
            if (!d) break;
            cmd.Dispatch(d->x, d->y, d->z);
            ++replayed;
            break;
        }
        case RHICommandType::CmdCopyBuffer: {
            const auto* d = stream.PayloadAt<RHICommandCopyBuffer>(i);
            if (!d || !d->src || !d->dst) break;
            cmd.CopyBuffer(static_cast<IRHIBuffer*>(d->src), static_cast<IRHIBuffer*>(d->dst),
                           d->size, d->srcOffset, d->dstOffset);
            ++replayed;
            break;
        }
        case RHICommandType::CmdBeginOffscreenPassMRT: {
            const auto* d = stream.PayloadAt<RHICommandBeginOffscreenPassMRT>(i);
            if (!d) break;
            cmd.SetRenderPassHint(d->renderPass);
            const auto* views = reinterpret_cast<void* const*>(
                stream.ArenaRawAt(d->colorViewsOffset, sizeof(void*) * d->colorCount));
            const auto* clear = d->clearCount
                ? reinterpret_cast<const ClearValue*>(stream.ArenaRawAt(d->clearOffset, sizeof(ClearValue) * d->clearCount))
                : nullptr;
            cmd.BeginOffscreenPassMRT(views, d->colorCount, d->depthImageView,
                                      d->width, d->height, clear, d->allowSecondary != 0u);
            ++replayed;
            break;
        }
        case RHICommandType::CmdDrawIndexedIndirect: {
            const auto* d = stream.PayloadAt<RHICommandDrawIndexedIndirect>(i);
            if (!d || !d->buffer) break;
            cmd.DrawIndexedIndirect(static_cast<IRHIBuffer*>(d->buffer), d->offset, d->drawCount, d->stride);
            ++replayed;
            break;
        }
        case RHICommandType::CmdDrawIndexedIndirectCount: {
            const auto* d = stream.PayloadAt<RHICommandDrawIndexedIndirectCount>(i);
            if (!d || !d->buffer || !d->countBuffer) break;
            cmd.DrawIndexedIndirectCount(static_cast<IRHIBuffer*>(d->buffer), d->offset,
                                         static_cast<IRHIBuffer*>(d->countBuffer), d->countOffset,
                                         d->maxDrawCount, d->stride);
            ++replayed;
            break;
        }
        case RHICommandType::CmdCopyTextureToBuffer: {
            const auto* d = stream.PayloadAt<RHICommandCopyTextureToBuffer>(i);
            if (!d || !d->texture || !d->buffer) break;
            cmd.CopyTextureToBuffer(static_cast<IRHITexture*>(d->texture),
                                    static_cast<IRHIBuffer*>(d->buffer),
                                    d->x, d->y, d->width, d->height, d->bufferOffset);
            ++replayed;
            break;
        }
        case RHICommandType::CmdClearDepthStencil: {
            const auto* d = stream.PayloadAt<RHICommandClearDepthStencil>(i);
            if (!d || !d->texture) break;
            cmd.ClearDepthStencil(static_cast<IRHITexture*>(d->texture), d->depth);
            ++replayed;
            break;
        }
        case RHICommandType::CmdBuildTLAS: {
            const auto* d = stream.PayloadAt<RHICommandBuildTLAS>(i);
            if (!d || !d->tlas) break;
            cmd.BuildTLAS(static_cast<IRHIAccelerationStructure*>(d->tlas),
                          static_cast<IRHIBuffer*>(d->scratch),
                          static_cast<IRHIBuffer*>(d->instances),
                          d->instanceCount, d->update != 0u);
            ++replayed;
            break;
        }
        case RHICommandType::CmdBindRTPipeline: {
            const auto* d = stream.PayloadAt<RHICommandBindRTPipeline>(i);
            if (!d || !d->rtPSO) break;
            cmd.BindRTPipeline(static_cast<IRHIRayTracingPipelineState*>(d->rtPSO));
            ++replayed;
            break;
        }
        case RHICommandType::CmdPipelineBarrierTexture: {
            const auto* d = stream.PayloadAt<RHICommandPipelineBarrierTexture>(i);
            if (!d || !d->texture) break;
            cmd.PipelineBarrier(static_cast<PipelineStage>(d->srcStage), static_cast<PipelineStage>(d->dstStage),
                                static_cast<ResourceState>(d->srcState), static_cast<ResourceState>(d->dstState),
                                static_cast<IRHITexture*>(d->texture));
            ++replayed;
            break;
        }
        case RHICommandType::CmdBufferBarrier: {
            const auto* d = stream.PayloadAt<RHICommandBufferBarrier>(i);
            if (!d || !d->buffer) break;
            cmd.BufferBarrier(static_cast<IRHIBuffer*>(d->buffer), static_cast<ResourceState>(d->srcState),
                              static_cast<ResourceState>(d->dstState));
            ++replayed;
            break;
        }
        case RHICommandType::CmdBeginDebugLabel:
        case RHICommandType::CmdInsertDebugLabel: {
            const auto* d = stream.PayloadAt<RHICommandSetDebugLabel>(i);
            if (!d) break;
            if (c.type == RHICommandType::CmdBeginDebugLabel) cmd.BeginDebugLabel(d->text);
            else                                              cmd.InsertDebugLabel(d->text);
            ++replayed;
            break;
        }
        case RHICommandType::CmdEndDebugLabel:
            cmd.EndDebugLabel();
            ++replayed;
            break;
        case RHICommandType::CmdClearDrawDebugLabel:
            cmd.ClearDrawDebugLabel();
            ++replayed;
            break;
        case RHICommandType::CmdTimelineSignal:
        case RHICommandType::CmdTimelineWait: {
            const auto* d = stream.PayloadAt<RHICommandTimeline>(i);
            if (!d) break;
            if (c.type == RHICommandType::CmdTimelineSignal) cmd.SetTimelineSignal(static_cast<RHIFenceHandle>(d->fence), d->value);
            else                                             cmd.SetTimelineWait(static_cast<RHIFenceHandle>(d->fence), d->value);
            ++replayed;
            break;
        }
        case RHICommandType::CmdWriteTimestamp:
        case RHICommandType::CmdBeginQuery:
        case RHICommandType::CmdEndQuery:
        case RHICommandType::CmdResetQueryPool: {
            const auto* d = stream.PayloadAt<RHICommandQuery>(i);
            if (!d || !d->pool) break;
            auto* pool = static_cast<IRHIQueryPool*>(d->pool);
            switch (c.type) {
                case RHICommandType::CmdWriteTimestamp: cmd.WriteTimestamp(pool, d->index); break;
                case RHICommandType::CmdBeginQuery:     cmd.BeginQuery(pool, d->index);     break;
                case RHICommandType::CmdEndQuery:       cmd.EndQuery(pool, d->index);       break;
                default:                                cmd.ResetQueryPool(pool);           break;
            }
            ++replayed;
            break;
        }
        case RHICommandType::CmdDrawMeshTasks: {
            const auto* d = stream.PayloadAt<RHICommandDrawMeshTasks>(i);
            if (!d) break;
            cmd.DrawMeshTasks(d->x, d->y, d->z);
            ++replayed;
            break;
        }
        case RHICommandType::CmdDrawMeshTasksIndirect: {
            const auto* d = stream.PayloadAt<RHICommandDrawIndexedIndirect>(i);
            if (!d || !d->buffer) break;
            cmd.DrawMeshTasksIndirect(static_cast<IRHIBuffer*>(d->buffer), d->offset, d->drawCount, d->stride);
            ++replayed;
            break;
        }
        case RHICommandType::CmdDrawMeshTasksIndirectCount: {
            const auto* d = stream.PayloadAt<RHICommandDrawIndexedIndirectCount>(i);
            if (!d || !d->buffer || !d->countBuffer) break;
            cmd.DrawMeshTasksIndirectCount(static_cast<IRHIBuffer*>(d->buffer), d->offset,
                                           static_cast<IRHIBuffer*>(d->countBuffer), d->countOffset,
                                           d->maxDrawCount, d->stride);
            ++replayed;
            break;
        }
        case RHICommandType::CmdExecuteGeneratedCommands: {
            const auto* d = stream.PayloadAt<RHICommandExecuteGeneratedCommands>(i);
            if (!d) break;
            IRHICommandList::DGCExecuteDesc desc{};
            desc.indirectCommandsLayout = d->indirectCommandsLayout;
            desc.indirectExecutionSet   = d->indirectExecutionSet;
            desc.sequencesBufferAddr    = d->sequencesBufferAddr;
            desc.maxSequenceCount       = d->maxSequenceCount;
            desc.sequenceCountAddr      = d->sequenceCountAddr;
            desc.preprocessBufferAddr   = d->preprocessBufferAddr;
            desc.preprocessBufferSize   = d->preprocessBufferSize;
            desc.maxDrawCount           = d->maxDrawCount;
            cmd.ExecuteGeneratedCommands(desc);
            ++replayed;
            break;
        }
        // 【P3】命令流承载光追：BLAS 构建 / 光线派发按录制期复制进流的数据在回放侧下发
        case RHICommandType::CmdBuildBLAS: {
            // 【P3 根因修复】BLAS 构建是录制侧用 `EnqueueArena` 编码的（载荷在 arena、
            // 内联 payloadSize 为 0）。此前这里用 `PayloadAt`（只读内联载荷）取，长度校验
            // 必然失败返回 nullptr ⇒ 整条构建命令被静默跳过：TLAS 里的实例指向从未构建过的
            // BLAS ⇒ 所有光线未命中、画面只剩天空，且不产生任何 D3D12 错误（跳过命令是合法的）。
            // 必须与录制侧成对使用 `ArenaPayloadAt`。
            const auto* d = stream.ArenaPayloadAt<RHICommandBuildBLAS>(i);
            if (!d || !d->blas || d->geometryCount == 0u) {
                // 不允许静默丢命令：载荷读不出来时留一条明确日志（限一次，避免刷屏）
                static bool s_warnedBadBLAS = false;
                if (!s_warnedBadBLAS) {
                    s_warnedBadBLAS = true;
                    HE_CORE_WARN("ReplayCommandStream: CmdBuildBLAS 载荷不可读（内联/arena 长度不匹配），"
                                 "该 BLAS 构建被跳过");
                }
                break;
            }
            const auto* geos = reinterpret_cast<const RHIBLASGeometry*>(
                stream.ArenaRawAt(d->geometryOffset, sizeof(RHIBLASGeometry) * d->geometryCount));
            if (!geos) break;
            BLASBuildDesc desc{};
            desc.geometries.resize(d->geometryCount);
            for (u32 gi = 0; gi < d->geometryCount; ++gi) {
                const RHIBLASGeometry& src = geos[gi];
                RTGeometryDesc& dst = desc.geometries[gi];
                dst.type              = static_cast<RTGeometryType>(src.type);
                dst.vertexBuffer      = static_cast<IRHIBuffer*>(src.vertexBuffer);
                dst.vertexFormat      = static_cast<Format>(src.vertexFormat);
                dst.vertexStride      = src.vertexStride;
                dst.maxVertex         = src.maxVertex;
                dst.indexBuffer       = static_cast<IRHIBuffer*>(src.indexBuffer);
                dst.indexFormat       = static_cast<Format>(src.indexFormat);
                dst.maxPrimitiveCount = src.maxPrimitiveCount;
                dst.transformBuffer   = static_cast<IRHIBuffer*>(src.transformBuffer);
            }
            desc.flags = static_cast<ASBuildFlags>(static_cast<u8>(d->flags));
            cmd.BuildBLAS(static_cast<IRHIAccelerationStructure*>(d->blas),
                          static_cast<IRHIBuffer*>(d->scratch), desc, d->update != 0u);
            ++replayed;
            break;
        }
        case RHICommandType::CmdTraceRays: {
            const auto* d = stream.ArenaPayloadAt<RHICommandTraceRays>(i);
            if (!d || !d->buffer) break;
            SBTDesc sbt{};
            sbt.buffer          = static_cast<IRHIBuffer*>(d->buffer);
            sbt.rayGen.handleOffset = d->rayGenOffset;  sbt.rayGen.stride = d->rayGenStride;
            sbt.miss.handleOffset   = d->missOffset;    sbt.miss.stride   = d->missStride;
            sbt.hit.handleOffset    = d->hitOffset;     sbt.hit.stride    = d->hitStride;
            sbt.callable.handleOffset = d->callableOffset;
            sbt.callable.stride       = d->callableStride;
            cmd.TraceRays(sbt, d->width, d->height, d->depth);
            ++replayed;
            break;
        }
        // 【T10】录制期已解析好的后端细节：回放只按数据下指令，不再查描述符集 / 设备状态表
        case RHICommandType::CmdResolvedBarrier: {
            const auto* d = stream.PayloadAt<RHICommandResolvedBarrier>(i);
            if (!d) break;
            // 类型数值与 IRHICommandList::ResolvedBarrierType 一一对应
            cmd.ApplyResolvedBarrier(d->resource,
                                     static_cast<IRHICommandList::ResolvedBarrierType>(d->type),
                                     d->subresource, d->before, d->after);
            ++replayed;
            break;
        }
        case RHICommandType::CmdResolvedDescriptorTables: {
            const auto* d = stream.PayloadAt<RHICommandDescriptorTables>(i);
            if (!d) break;
            cmd.ApplyResolvedDescriptorTable(d->setIndex, d->set, d->srv, d->uav, d->sampler);
            ++replayed;
            break;
        }
        case RHICommandType::CmdResolvedRenderTargets: {
            const auto* d = stream.PayloadAt<RHICommandResolvedRenderTargets>(i);
            if (!d) break;
            const auto* rtvs = d->count
                ? reinterpret_cast<void* const*>(stream.ArenaRawAt(d->rtvsOffset, sizeof(void*) * d->count))
                : nullptr;
            if (d->count && !rtvs) break;
            cmd.ApplyResolvedRenderTargets(rtvs, d->count, d->dsv);
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
