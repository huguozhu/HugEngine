// Pipeline/GBufferRenderer_GPU.cpp — GPU Driven GBuffer 渲染
// DrawIndexedIndirect + SV_InstanceID 模式
// MeshBatcher::Build + FillGPUScene 在 DeferredPipeline::BuildFrameGraph 中完成
#include "Pipeline/GBufferRenderer_GPU.h"
#include "Pipeline/MeshBatcher.h"
#include "Scene/MeshComponent.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Core/Log.h"
#include <cstdio>

namespace he::render {

bool GBufferRenderer_GPU::Initialize(GBufferContext& ctx) {
    (void)ctx;
    return true;
}

void GBufferRenderer_GPU::Shutdown() {}

void GBufferRenderer_GPU::Render(rhi::IRHICommandList* cmd, GBufferContext& ctx,
                                  he::World& world, he::SceneGraph& sg,
                                  const CameraData& camera) {
    // MeshBatcher::Build + FillGPUScene 已在 BuildFrameGraph 中完成（Upload 之前）
    // 上传 ObjectBuffer 并获取 DrawItem 列表（GPU 路径用不到但 CPU 回退需要）
    auto drawItems = ctx.sceneRenderer->Prepare(world, sg, camera, ctx.objectBuffer);

    u32 w = ctx.width, h = ctx.height;

    // 推送 bindless 纹理（Flush 自动遍历全部已注册描述符集）
    ctx.device->GetBindlessHeap()->Flush();

    // 绑定 set=0
    ctx.device->UpdateDescriptorSet(ctx.descSet, rhi::kBindingObjectData, rhi::DescriptorType::StorageBuffer,
                                     ctx.objectBuffer);
    cmd->SetPipeline(ctx.pso);
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, ctx.descSet);

    // 清除 + 开始 MRT（7 颜色 + 深度）
    rhi::ClearValue clears[8]{};
    clears[0].color[3] = 1.0f; clears[1].color[3] = 1.0f;
    clears[2].color[3] = 1.0f; clears[3].color[0] = 0.0f;
    clears[3].color[1] = 0.0f;
    clears[5].color[2] = 0.5f; clears[5].color[3] = 0.0f;  // disneyA: specular=0.5, sheen=0（中性默认）
    clears[6].color[1] = 1.0f; clears[6].color[2] = 1.0f;  // disneyB: clearcoatGloss=1, specularTint.r=1
    clears[6].color[3] = 1.0f;                              // disneyB: specularTint.g=1
    clears[7].depth = 1.0f;
    void* cv[7] = { ctx.gbA->GetNativeHandle(), ctx.gbB->GetNativeHandle(),
                    ctx.gbC->GetNativeHandle(), ctx.gbVel->GetNativeHandle(),
                    ctx.gbWorldPos->GetNativeHandle(), ctx.gbDisneyA->GetNativeHandle(),
                    ctx.gbDisneyB->GetNativeHandle() };
    cmd->BeginOffscreenPassMRT(cv, 7, ctx.gbDepth->GetNativeHandle(), w, h, clears, false);
    cmd->SetViewport({0, (float)h, (float)w, -(float)h, 0, 1});
    cmd->SetScissor({0, 0, w, h});

    // GPU Driven 绘制
    u32 visCount = ctx.gpuCulling->GetLastVisibleCount();
    u32 totalIdx = ctx.meshBatcher ? ctx.meshBatcher->GetTotalIndexCount() : 0;
    static int dbgCount = 0;
    if (++dbgCount <= 3)
        HE_CORE_INFO("GBuffer_GPU frame={}: visCount={}, totalIdx={}, vb={}, ib={}",
            dbgCount, visCount, totalIdx,
            (void*)(ctx.meshBatcher ? ctx.meshBatcher->GetVertexBuffer() : nullptr),
            (void*)(ctx.meshBatcher ? ctx.meshBatcher->GetIndexBuffer() : nullptr));

    // 仅 GPU Culling 启用时才走 IndirectDraw 路径，避免禁用时使用脏数据
    if (ctx.gpuCulling->enabled && visCount > 0 && totalIdx > 0) {
        cmd->SetVertexBuffer(ctx.meshBatcher->GetVertexBuffer(), 0);
        cmd->SetIndexBuffer(ctx.meshBatcher->GetIndexBuffer(), 0);

        struct {
            float4x4 viewProjMatrix;
            float4x4 prevViewProjMatrix;
            u32      objectIndex;
            u32      useInstanceID;
            u32      _pad[14];
        } pc;
        pc.viewProjMatrix     = camera.GetViewProjMatrix();
        pc.prevViewProjMatrix = ctx.prevViewProj;
        pc.objectIndex        = 0;
        pc.useInstanceID      = 1;

        cmd->SetPushConstants(0, sizeof(pc), &pc);

        // ── DGC 路径 vs 传统 ExecuteIndirect 路径 ──
        if (ctx.dgc.enabled && ctx.dgc.sequenceBuffer && ctx.dgc.countBuffer) {
            // DGC 路径：由 GPU 生成 vkCmdDrawIndexed 命令
            rhi::IRHICommandList::DGCExecuteDesc dgcDesc{};
            dgcDesc.indirectCommandsLayout = ctx.dgc.indirectCommandsLayout;
            dgcDesc.indirectExecutionSet   = ctx.dgc.indirectExecutionSet;
            dgcDesc.sequencesBufferAddr    = ctx.dgc.sequenceBuffer->GetDeviceAddress();
            dgcDesc.maxSequenceCount       = ctx.dgc.maxSequenceCount;
            dgcDesc.sequenceCountAddr      = ctx.dgc.countBuffer->GetDeviceAddress();
            dgcDesc.preprocessBufferAddr   = ctx.dgc.preprocessBufferAddr;
            dgcDesc.preprocessBufferSize   = ctx.dgc.preprocessBufferSize;
            dgcDesc.maxDrawCount           = visCount;

            // DrawCall 调试 marker（RenderDoc 定位用）
            char label[64];
            snprintf(label, sizeof(label), "GBuffer DGC (%u)", visCount);
            cmd->SetDrawDebugLabel(label);
            cmd->ExecuteGeneratedCommands(dgcDesc);
        } else {
            // 传统 ExecuteIndirect 路径：CPU 调用 vkCmdDrawIndexedIndirect
            char label[64];
            snprintf(label, sizeof(label), "GBuffer Indirect (%u)", visCount);
            cmd->SetDrawDebugLabel(label);
            cmd->DrawIndexedIndirect(ctx.gpuCulling->GetIndirectBuffer(), 0,
                                      visCount, sizeof(IndirectDrawCommand));
        }
    } else {
        // 回退到 CPU 逐对象绘制（首帧或 GPU 剔除异常时）
        if (dbgCount <= 3) HE_CORE_INFO("GBuffer_GPU: fallback CPU path, {} drawItems", drawItems.size());
        float4x4 jvp = camera.GetViewProjMatrix();
        for (auto& di : drawItems) {
            struct { float4x4 vp; float4x4 pvp; u32 oi; u32 uid; u32 _pad[14]; } pc;
            pc.vp = jvp; pc.pvp = ctx.prevViewProj; pc.oi = di.objectIndex; pc.uid = 0;
            // DrawCall 调试 marker：标记当前绘制的物体（RenderDoc 定位用）
            char label[64];
            snprintf(label, sizeof(label), "GBuffer Obj#%u", di.objectIndex);
            cmd->SetDrawDebugLabel(label);
            cmd->SetPushConstants(0, sizeof(pc), &pc);
            cmd->SetVertexBuffer(di.mesh->GetVertexBuffer().get(), 0);
            cmd->SetIndexBuffer(di.mesh->GetIndexBuffer().get());
            cmd->DrawIndexed(di.mesh->GetIndexCount());
        }
    }

    cmd->EndOffscreenPass();
}

} // namespace he::render
