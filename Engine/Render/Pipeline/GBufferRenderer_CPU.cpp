// Pipeline/GBufferRenderer_CPU.cpp — CPU Driven GBuffer 渲染
// 从 DeferredPipeline::BuildFrameGraph 提取的逐对象绘制逻辑
#include "Pipeline/GBufferRenderer_CPU.h"
#include "Pipeline/InstanceCuller.h"   // 任务 25：逐实例剔除
#include "Scene/InstancedMeshComponent.h"
#include "Scene/MeshComponent.h"
#include "Scene/World.h"
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

    // 清除值（8 颜色 MRT + 深度）
    rhi::ClearValue clears[9]{};
    clears[0].color[3] = 1.0f;
    clears[1].color[3] = 1.0f;
    clears[2].color[3] = 1.0f;
    clears[3].color[0] = 0.0f;
    // velocity=0;
    clears[3].color[1] = 0.0f;
    clears[5].color[2] = 0.5f;
    clears[5].color[3] = 0.0f;
    // disneyA: specular=0.5, sheen=0（中性默认）;
    clears[6].color[1] = 1.0f;
    clears[6].color[2] = 1.0f;
    // disneyB: clearcoatGloss=1, specularTint.r=1;
    clears[6].color[3] = 1.0f;                              // disneyB: specularTint.g=1
    // 光照图键（MRT7）：清除值 = (0,0,0,0)，天空像素的 z 分量 0 表示"页号无效"
    clears[7].color[3] = 0.0f;
    clears[8].depth = 1.0f;

    void* cv[8] = { ctx.gbA->GetNativeHandle(), ctx.gbB->GetNativeHandle(),
                    ctx.gbC->GetNativeHandle(), ctx.gbVel->GetNativeHandle(),
                    ctx.gbWorldPos->GetNativeHandle(), ctx.gbDisneyA->GetNativeHandle(),
                    ctx.gbDisneyB->GetNativeHandle(), ctx.gbLightmapKey->GetNativeHandle() };
    cmd->BeginOffscreenPassMRT(cv, 8, ctx.gbDepth->GetNativeHandle(), w, h, clears, false);
    cmd->SetViewport({0, (float)h, (float)w, -(float)h, 0, 1});
    cmd->SetScissor({0, 0, w, h});

    // SceneRenderer 准备所有绘制项（任务 24：Deferred 排除贴花卡片，改由 DecalPass 投影）
    auto drawItems = ctx.sceneRenderer->Prepare(world, sg, camera, ctx.objectBuffer, ctx.excludeDecals);

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
        // 任务 25：实例化网格由下面专门的实例化绘制段处理（SV_InstanceID 取实例变换）
        if (di.bInstanced) continue;
        struct {
            float4x4 viewProjMatrix;
            float4x4 prevViewProjMatrix;
            u32      objectIndex;
            u32      useInstanceID;   // 必须显式设为 0，匹配 shader 布局
            u32      instanceSSBOHandle;
            u32      instanceVisibleHandle;
            u32      _pad[12];
        } pc;
        pc.viewProjMatrix     = jitteredVP;
        pc.prevViewProjMatrix = ctx.prevViewProj;
        pc.objectIndex        = di.objectIndex;
        pc.useInstanceID      = 0;
        pc.instanceSSBOHandle = 0;
        pc.instanceVisibleHandle = 0;
        // DrawCall 调试 marker：标记当前绘制的物体（RenderDoc 定位用）
        char label[64];
        snprintf(label, sizeof(label), "GBuffer Obj#%u", di.objectIndex);
        cmd->SetDrawDebugLabel(label);
        cmd->SetPushConstants(0, sizeof(pc), &pc);
        cmd->SetVertexBuffer(di.mesh->GetVertexBuffer().get(), 0);
        cmd->SetIndexBuffer(di.mesh->GetIndexBuffer().get());
        cmd->DrawIndexed(di.mesh->GetIndexCount());
    }

    // ── 实例化网格（任务 25）：逐实例剔除 + 间接绘制 ──
    // 与 Forward 路径同一套机制：本组件的实例变换过六平面测试 → 可见列表 + 命令计数 →
    // DrawIndexedIndirect + SV_InstanceID 查可见列表。
    world.ForEach<he::InstancedMeshComponent>([&](he::Entity, he::InstancedMeshComponent& im) {
        if (im.GetInstanceCount() == 0 || im.GetIndexCount() == 0) return;
        // 定位对象条目（材质/世界变换），与 Forward 一致：一个对象条目服务 N 个实例
        u32 objIndex = 0;
        bool found = false;
        for (auto& di : filteredItems) {
            if (di.mesh == static_cast<he::MeshComponent*>(&im)) { objIndex = di.objectIndex; found = true; break; }
        }
        if (!found) return;

        const u32 count = im.GetInstanceCount();
        const u32 slot  = ctx.frameSlot % rhi::kMaxFramesInFlight;

        // 实例变换上传（与 Forward 路径共用同一份逻辑）：容量够就原地复用
        const u32 instHandle = ctx.instanceCuller
                             ? ctx.instanceCuller->UploadInstanceTransforms(ctx.device, im)
                             : 0;
        if (instHandle == 0) return;

        struct {
            float4x4 viewProjMatrix;
            float4x4 prevViewProjMatrix;
            u32      objectIndex;
            u32      useInstanceID;
            u32      instanceSSBOHandle;
            u32      instanceVisibleHandle;
            u32      _pad[12];
        } pc;
        pc.viewProjMatrix        = jitteredVP;
        pc.prevViewProjMatrix    = ctx.prevViewProj;
        pc.objectIndex           = objIndex;
        pc.useInstanceID         = 2;
        pc.instanceSSBOHandle    = im.instanceSSBOHandle;
        pc.instanceVisibleHandle = 0;

        // 逐实例剔除（仅在开关打开 + 剔除器可用时；否则整批绘制，行为与原 MVP 一致）
        bool useCull = im.enableFrustumCull && ctx.instanceCuller && ctx.instanceCuller->GetPSO();
        if (useCull) {
            if (!im.instanceCullCmd[slot]) {
                im.instanceCullCmd[slot] = ctx.instanceCuller->CreateCommandBuffer(im.GetIndexCount(), 0, 0);
                if (im.instanceCullCmd[slot]) {
                    im.instanceCullCmdHandle[slot] =
                        ctx.device->GetBindlessHeap()->RegisterBuffer(im.instanceCullCmd[slot].get());
                }
            }
            if (!im.instanceCullCmd[slot] || im.instanceCullCmdHandle[slot] == 0) useCull = false;
        }
        if (useCull) {
            const AABB lb = im.GetBounds();
            const u32 prevVisible = ctx.instanceCuller->Cull(
                cmd, im.instanceBuffer.get(), im.instanceSSBOHandle,
                im.instanceCullCmd[slot].get(), im.instanceCullCmdHandle[slot],
                count, slot, lb.min, lb.max, jitteredVP);
            pc.instanceVisibleHandle = ctx.instanceCuller->GetVisibleIndicesHandle(slot);
            // 首帧诊断：确认 Deferred 这条路径确实进来并拿到有效句柄
            static bool s_DbgOnce = false;
            if (!s_DbgOnce) {
                s_DbgOnce = true;
                HE_CORE_INFO("[任务 25] Deferred 实例化绘制首帧：实例 {}（对象 #{}），剔除槽 {}，"
                             "实例 SSBO 句柄 {}，命令句柄 {}，可见列表句柄 {}，上次可见 {}",
                             count, objIndex, slot, im.instanceSSBOHandle,
                             im.instanceCullCmdHandle[slot], pc.instanceVisibleHandle, prevVisible);
            }
            cmd->SetDrawDebugLabel("GBuffer InstancedMesh (逐实例剔除)");
            cmd->SetPushConstants(0, sizeof(pc), &pc);
            cmd->SetVertexBuffer(im.GetVertexBuffer().get(), 0);
            cmd->SetIndexBuffer(im.GetIndexBuffer().get());
            cmd->DrawIndexedIndirect(im.instanceCullCmd[slot].get(), 0, 1,
                                     sizeof(InstanceIndirectCommand));
            im.visibleInstanceCount = prevVisible;
            return;
        }

        cmd->SetDrawDebugLabel("GBuffer InstancedMesh");
        cmd->SetPushConstants(0, sizeof(pc), &pc);
        cmd->SetVertexBuffer(im.GetVertexBuffer().get(), 0);
        cmd->SetIndexBuffer(im.GetIndexBuffer().get());
        cmd->DrawIndexed(im.GetIndexCount(), count);
    });

    cmd->EndOffscreenPass();
}

} // namespace he::render
