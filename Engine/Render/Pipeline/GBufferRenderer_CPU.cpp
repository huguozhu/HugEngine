// Pipeline/GBufferRenderer_CPU.cpp — CPU Driven GBuffer 渲染
// 从 DeferredPipeline::BuildFrameGraph 提取的逐对象绘制逻辑
#include "Pipeline/GBufferRenderer_CPU.h"
#include "Scene/MeshComponent.h"
#include "Core/Log.h"
#include <unordered_set>
#include <cstdio>

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

    // 推送 bindless 纹理到全部已注册描述符集（Flush 自动遍历全部 set）
    ctx.device->GetBindlessHeap()->Flush();

    // 绑定 set=0（per-frame ObjectBuffer + bindless 纹理/采样器数组）
    ctx.device->UpdateDescriptorSet(ctx.descSet, rhi::kBindingObjectData, rhi::DescriptorType::StorageBuffer,
                                     ctx.objectBuffer);
    cmd->SetPipeline(ctx.pso);
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, ctx.descSet);

    // 清除值（7 颜色 MRT + 深度）
    rhi::ClearValue clears[8]{};
    clears[0].color[3] = 1.0f; clears[1].color[3] = 1.0f;
    clears[2].color[3] = 1.0f; clears[3].color[0] = 0.0f; // velocity=0
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

    // SceneRenderer 准备所有绘制项
    auto drawItems = ctx.sceneRenderer->Prepare(world, sg, camera, ctx.objectBuffer);

    // GPU 剔除过滤（Readback 上帧结果 → 过滤可见物体）
    // 仅 GPU Culling 启用且 visIndices 非空时才过滤，避免使用脏数据
    const auto& visIndices = *ctx.gpuVisibleIndices;
    bool useGPUVisible = ctx.gpuCulling->enabled && !visIndices.empty();
    std::vector<DrawItem> filteredItems;
    bool gpuCullSafe = useGPUVisible
        && visIndices.size() <= drawItems.size()
        && ctx.gpuScene->GetObjectCount() == (u32)drawItems.size();
    if (gpuCullSafe) {
        std::unordered_set<u32> visSet(visIndices.begin(), visIndices.end());
        for (auto& di : drawItems)
            if (visSet.count(di.objectIndex)) filteredItems.push_back(di);

        // 调试：每 120 帧输出 GPU 剔除统计
        static int gpuCullDbgFrame = 0;
        if (ctx.gpuCulling->enabled && ++gpuCullDbgFrame % 120 == 0) {
            std::string visList, cullList;
            for (auto& di : drawItems) {
                if (visSet.count(di.objectIndex))
                    visList += std::to_string(di.objectIndex) + " ";
                else
                    cullList += std::to_string(di.objectIndex) + " ";
            }
            HE_CORE_INFO("GPU Cull frame={}: {}/{} visible, culled=[{}]",
                gpuCullDbgFrame, filteredItems.size(), drawItems.size(),
                cullList.empty() ? "none" : cullList);
        }
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
            u32      useInstanceID;   // 必须显式设为 0，匹配 shader 布局
            u32      _pad[14];
        } pc;
        pc.viewProjMatrix     = jitteredVP;
        pc.prevViewProjMatrix = ctx.prevViewProj;
        pc.objectIndex        = di.objectIndex;
        pc.useInstanceID      = 0;
        // DrawCall 调试 marker：标记当前绘制的物体（RenderDoc 定位用）
        char label[64];
        snprintf(label, sizeof(label), "GBuffer Obj#%u", di.objectIndex);
        cmd->SetDrawDebugLabel(label);
        cmd->SetPushConstants(0, sizeof(pc), &pc);
        cmd->SetVertexBuffer(di.mesh->GetVertexBuffer().get(), 0);
        cmd->SetIndexBuffer(di.mesh->GetIndexBuffer().get());
        cmd->DrawIndexed(di.mesh->GetIndexCount());
    }

    cmd->EndOffscreenPass();
}

} // namespace he::render
