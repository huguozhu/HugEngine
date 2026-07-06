// Pipeline/GBufferRenderer_GPU.cpp — GPU Driven GBuffer 渲染
// DrawIndexedIndirect + SV_InstanceID 模式
// MeshBatcher::Build + FillGPUScene 在 DeferredPipeline::BuildFrameGraph 中完成
#include "Pipeline/GBufferRenderer_GPU.h"
#include "Pipeline/MeshBatcher.h"
#include "Asset/BindlessTextureManager.h"
#include "Core/Log.h"

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
    // 上传 ObjectBuffer（GPU 模式仍需 per-object transform 数据）
    ctx.sceneRenderer->Prepare(world, sg, camera, ctx.objectBuffer);

    u32 w = ctx.width, h = ctx.height;

    // 推送 bindless 纹理
    he::asset::BindlessTextureManager::Instance().FlushPending();

    // 绑定 set=0
    ctx.device->UpdateDescriptorSet(ctx.descSet, 2, rhi::DescriptorType::StorageBuffer,
                                     ctx.objectBuffer);
    cmd->SetPipeline(ctx.pso);
    cmd->BindDescriptorSet(0, ctx.descSet);

    // 清除 + 开始 MRT
    rhi::ClearValue clears[5]{};
    clears[0].color[3] = 1.0f; clears[1].color[3] = 1.0f;
    clears[2].color[3] = 1.0f; clears[3].color[0] = 0.0f;
    clears[3].color[1] = 0.0f; clears[4].depth = 1.0f;
    void* cv[4] = { ctx.gbA->GetNativeHandle(), ctx.gbB->GetNativeHandle(),
                    ctx.gbC->GetNativeHandle(), ctx.gbVel->GetNativeHandle() };
    cmd->BeginOffscreenPassMRT(cv, 4, ctx.gbDepth->GetNativeHandle(), w, h, clears, false);
    cmd->SetViewport({0, (float)h, (float)w, -(float)h, 0, 1});
    cmd->SetScissor({0, 0, w, h});

    // GPU Driven 绘制：合并后的 VB/IB + Indirect command buffer
    u32 visCount = ctx.gpuCulling->GetLastVisibleCount();
    u32 totalIdx = ctx.meshBatcher ? ctx.meshBatcher->GetTotalIndexCount() : 0;
    if (visCount > 0 && totalIdx > 0) {
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
        cmd->DrawIndexedIndirect(ctx.gpuCulling->GetIndirectBuffer(), 0,
                                  visCount, sizeof(IndirectDrawCommand));
    }

    cmd->EndOffscreenPass();
}

} // namespace he::render
