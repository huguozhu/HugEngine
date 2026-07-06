// Pipeline/GBufferRenderer_CPU.cpp — CPU Driven GBuffer 渲染
// 从 DeferredPipeline::BuildFrameGraph 提取的逐对象绘制逻辑
#include "Pipeline/GBufferRenderer_CPU.h"
#include "Asset/BindlessTextureManager.h"
#include "Scene/MeshComponent.h"
#include <unordered_set>

namespace he::render {

bool GBufferRenderer_CPU::Initialize(GBufferContext& ctx) {
    (void)ctx;  // CPU 模式无需额外初始化
    return true;
}

void GBufferRenderer_CPU::Shutdown() {
    // CPU 模式无需额外清理（纹理/PSO 由 DeferredPipeline 管理）
}

void GBufferRenderer_CPU::Render(rhi::IRHICommandList* cmd, GBufferContext& ctx,
                                  he::World& world, he::SceneGraph& sg,
                                  const CameraData& camera) {
    u32 w = ctx.width, h = ctx.height;

    // 推送 bindless 纹理到全部已注册描述符集
    he::asset::BindlessTextureManager::Instance().FlushPending();

    // 绑定 set=0（per-frame ObjectBuffer + bindless 纹理/采样器数组）
    ctx.device->UpdateDescriptorSet(ctx.descSet, 2, rhi::DescriptorType::StorageBuffer,
                                     ctx.objectBuffer);
    cmd->SetPipeline(ctx.pso);
    cmd->BindDescriptorSet(0, ctx.descSet);

    // 清除值（4 颜色 MRT + 深度）
    rhi::ClearValue clears[5]{};
    clears[0].color[3] = 1.0f; clears[1].color[3] = 1.0f;
    clears[2].color[3] = 1.0f; clears[3].color[0] = 0.0f; // velocity=0
    clears[3].color[1] = 0.0f; clears[4].depth = 1.0f;

    void* cv[4] = { ctx.gbA->GetNativeHandle(), ctx.gbB->GetNativeHandle(),
                    ctx.gbC->GetNativeHandle(), ctx.gbVel->GetNativeHandle() };
    cmd->BeginOffscreenPassMRT(cv, 4, ctx.gbDepth->GetNativeHandle(), w, h, clears, false);
    cmd->SetViewport({0, (float)h, (float)w, -(float)h, 0, 1});
    cmd->SetScissor({0, 0, w, h});

    // SceneRenderer 准备所有绘制项
    auto drawItems = ctx.sceneRenderer->Prepare(world, sg, camera, ctx.objectBuffer);

    // GPU 剔除过滤（Readback 上帧结果 → 过滤可见物体）
    const auto& visIndices = *ctx.gpuVisibleIndices;
    bool useGPUVisible = !visIndices.empty();
    std::vector<DrawItem> filteredItems;
    bool gpuCullSafe = useGPUVisible
        && visIndices.size() <= drawItems.size()
        && ctx.gpuScene->GetObjectCount() == (u32)drawItems.size();
    if (gpuCullSafe) {
        std::unordered_set<u32> visSet(visIndices.begin(), visIndices.end());
        for (auto& di : drawItems)
            if (visSet.count(di.objectIndex)) filteredItems.push_back(di);
    } else {
        filteredItems = std::move(drawItems);
    }

    // 逐对象绘制（push constant objectIndex 模式）
    float4x4 jitteredVP = camera.GetViewProjMatrix();
    for (auto& di : filteredItems) {
        struct {
            float4x4 viewProjMatrix;
            float4x4 prevViewProjMatrix;
            u32      objectIndex;
            u32      _pad[15];
        } pc;
        pc.viewProjMatrix     = jitteredVP;
        pc.prevViewProjMatrix = ctx.prevViewProj;
        pc.objectIndex        = di.objectIndex;
        cmd->SetPushConstants(0, sizeof(pc), &pc);
        cmd->SetVertexBuffer(di.mesh->GetVertexBuffer().get(), 0);
        cmd->SetIndexBuffer(di.mesh->GetIndexBuffer().get());
        cmd->DrawIndexed(di.mesh->GetIndexCount());
    }

    cmd->EndOffscreenPass();
}

} // namespace he::render
