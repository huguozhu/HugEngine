// Pipeline/GBufferRenderer_CPU.cpp — CPU Driven GBuffer 渲染
// 从 DeferredPipeline::BuildFrameGraph 提取的逐对象绘制逻辑
#include "Pipeline/GBufferRenderer_CPU.h"
#include "Pipeline/ParallelRecordHelper.h"   // T3.2：GBuffer 的并行录制骨架
#include "Threading/JobSystem.h"             // T3.2：按线程数建录制池
#include "Pipeline/InstanceCuller.h"   // 任务 25：逐实例剔除（状态表也在它这里）
#include "Threading/MeshRegistry.h"    // 阶段 1 第①段：按 meshIndex 取顶点/索引缓冲
#include "Scene/MeshComponent.h"
#include "Scene/World.h"
#include "Core/Log.h"
#include <unordered_set>
#include <cstdio>
#include <algorithm>   // T3.2：std::min/std::max

namespace he::render {

bool GBufferRenderer_CPU::Initialize(GBufferContext& ctx) {
    (void)ctx;  // CPU 模式无需额外初始化
    return true;
}

void GBufferRenderer_CPU::Shutdown() {
    // CPU 模式无需额外清理（纹理/PSO 由 DeferredPipeline 管理）
}

void GBufferRenderer_CPU::Render(rhi::IRHICommandList* cmd, GBufferContext& ctx,
                                  const FrameSceneSnapshot& snapshot,
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
    // T3.2：本 pass 内会 ExecuteSecondary（并行录制）⇒ 必须允许 INLINE_AND_SECONDARY
    cmd->BeginOffscreenPassMRT(cv, 8, ctx.gbDepth->GetNativeHandle(), w, h, clears, /*allowSecondary=*/true);
    cmd->SetViewport({0, (float)h, (float)w, -(float)h, 0, 1});
    cmd->SetScissor({0, 0, w, h});

    // SceneRenderer 准备所有绘制项（阶段 1 §15.1 第②段：改吃快照，不再遍历世界；
    // 贴花是否排除已在收集侧由 `SceneSnapshotObjectOptions` 决定 ⇒ 这里不再传 excludeDecals）
    auto drawItems = ctx.sceneRenderer->Prepare(snapshot, camera, ctx.objectBuffer);

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
    // 【单个 draw 的录制】串行与并行两条路径共用同一份，保证两种模式录出来的命令逐条一致。
    const auto recordOne = [&](rhi::IRHICommandList* c, const DrawItem& di) {
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
        c->SetDrawDebugLabel(label);
        c->SetPushConstants(0, sizeof(pc), &pc);
        // 顶点/索引缓冲按 `meshIndex` 从注册表取（第②段：`DrawItem` 不再持有组件指针）
        const MeshRegistryEntry* me = ctx.meshRegistry ? ctx.meshRegistry->Find(di.meshIndex) : nullptr;
        if (!me || !me->vertexBuffer || !me->indexBuffer) return;
        c->SetVertexBuffer(me->vertexBuffer, 0);
        c->SetIndexBuffer(me->indexBuffer);
        c->DrawIndexed(me->indexCount);
    };

    // 【T3.2】CPU 路径的逐对象绘制改**并行录制**：池 / 分块 / 归属声明 / 按块序合并都在
    // `ParallelRecordHelper` 里；这里只提供"一块怎么录"。
    // 注意两点：① 次级列表**不继承**主列表的绑定 ⇒ 每块自己绑 per-frame 描述符集 + viewport/scissor；
    // ② 分块是 `filteredItems` 的连续升序区间 ⇒ 绘制顺序与串行完全一致（可逐位复现）。
    // 【T3.2】录制池是文件级单例：一个进程只有一套 GBuffer 次级列表池。
    // （`GBufferRenderer_CPU` 是独立于 `GBufferRenderer` 的类，故不挂在类成员上。）
    static ParallelRecordHelper s_ParallelRecorder;
    if (ctx.pso && !s_ParallelRecorder.IsReady()) {
        const u32 n = std::min<u32>(8u, std::max<u32>(1u, JobSystem::Instance().GetThreadCount()));
        s_ParallelRecorder.Initialize(ctx.device, n, "GBufferSecRecord");
    }
    if (ctx.pso && s_ParallelRecorder.IsReady() && !filteredItems.empty()) {
        s_ParallelRecorder.Record(cmd, static_cast<u32>(filteredItems.size()), ctx.pso,
            [&](rhi::IRHICommandList& secRef, u32 start, u32 end) {
                auto& s = secRef;
                s.BindDescriptorSet(rhi::kDescSetPerFrame, ctx.descSet);
                s.SetViewport({0, (float)h, (float)w, -(float)h, 0, 1});
                s.SetScissor({0, 0, w, h});
                for (u32 i = start; i < end; ++i) {
                    const DrawItem& di = filteredItems[i];
                    // 任务 25：实例化网格由下面专门的实例化绘制段处理（SV_InstanceID 取实例变换）
                    if (di.bInstanced) continue;
                    recordOne(&s, di);
                }
            });
    } else {
        for (auto& di : filteredItems) {
            // 任务 25：实例化网格由下面专门的实例化绘制段处理
            if (di.bInstanced) continue;
            recordOne(cmd, di);
        }
    }

    // ── 实例化网格（任务 25）：逐实例剔除 + 间接绘制 ──
    // 与 Forward 路径同一套机制：本组件的实例变换过六平面测试 → 可见列表 + 命令计数 →
    // DrawIndexedIndirect + SV_InstanceID 查可见列表。
    // 【阶段 1 第①段 / §15.1】数据来源改为**快照**：实例变换/开关/版本号按值带走，顶点与索引缓冲
    // 按 `meshIndex` 去注册表取，逐网格的 GPU 缓冲状态在 `InstanceCuller` 的实例状态表里 ——
    // 渲染期因此不再遍历世界、也不再读 `InstancedMeshComponent`。
    if (ctx.instanceCuller) {
        ctx.instanceCuller->BeginInstancesFrame(ctx.device);   // 帧边界：推进退役队列 + 回收上帧未见的条目
        const u32 slot = ctx.frameSlot % rhi::kMaxFramesInFlight;

        for (const SnapshotInstance& si : snapshot.instances) {
            if (si.transformCount == 0u) continue;             // 无实例：跳过（旧路径同样跳过）
            // 变换切片越界保护：快照损坏时宁可少画，也不要读越界内存
            if (static_cast<usize>(si.transformOffset) + si.transformCount >
                snapshot.instanceTransforms.size()) continue;
            const MeshRegistryEntry* me = ctx.meshRegistry ? ctx.meshRegistry->Find(si.meshIndex) : nullptr;
            if (!me || !me->vertexBuffer || !me->indexBuffer) continue;   // 未注册/已注销：跳过

            // 定位对象条目（材质/世界变换），与 Forward 一致：一个对象条目服务 N 个实例
            u32 objIndex = 0;
            bool found = false;
            for (auto& di : filteredItems) {
                // E-3②：按 meshIndex（整数）对齐对象条目 —— 组件地址比较已随组件指针一起退出
                if (si.meshIndex != 0u && si.meshIndex == di.meshIndex) {
                    objIndex = di.objectIndex; found = true; break;
                }
            }
            if (!found) continue;

            const u32 count = si.transformCount;
            // 实例变换上传（状态在渲染侧：容量够且版本未变 ⇒ 直接复用句柄）
            const u32 instHandle = ctx.instanceCuller->UploadInstanceTransforms(
                ctx.device, si.meshIndex,
                snapshot.instanceTransforms.data() + si.transformOffset,
                count, si.transformVersion, si.sourceEntity);
            if (instHandle == 0) continue;
            InstanceCuller::InstanceState* st = ctx.instanceCuller->FindInstanceState(si.meshIndex);
            if (!st) continue;

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
            pc.instanceSSBOHandle    = instHandle;
            pc.instanceVisibleHandle = 0;

            // 逐实例剔除（仅在开关打开 + 剔除器可用时；否则整批绘制，行为与原 MVP 一致）
            bool useCull = si.enableFrustumCull && ctx.instanceCuller->GetPSO();
            if (useCull) {
                if (!st->cullCmd[slot]) {
                    st->cullCmd[slot] = ctx.instanceCuller->CreateCommandBuffer(me->indexCount, 0, 0);
                    if (st->cullCmd[slot]) {
                        st->cullCmdHandle[slot] =
                            ctx.device->GetBindlessHeap()->RegisterBuffer(st->cullCmd[slot].get());
                    }
                }
                if (!st->cullCmd[slot] || st->cullCmdHandle[slot] == 0) useCull = false;
            }
            if (useCull) {
                const u32 prevVisible = ctx.instanceCuller->Cull(
                    cmd, st->buffer.get(), st->ssboHandle,
                    st->cullCmd[slot].get(), st->cullCmdHandle[slot],
                    count, slot, si.localBoundsMin, si.localBoundsMax, jitteredVP);
                pc.instanceVisibleHandle = ctx.instanceCuller->GetVisibleIndicesHandle(slot);
                // 首帧诊断：确认 Deferred 这条路径确实进来并拿到有效句柄
                static bool s_DbgOnce = false;
                if (!s_DbgOnce) {
                    s_DbgOnce = true;
                    HE_CORE_INFO("[任务 25] Deferred 实例化绘制首帧：实例 {}（对象 #{}），剔除槽 {}，"
                                 "实例 SSBO 句柄 {}，命令句柄 {}，可见列表句柄 {}，上次可见 {}",
                                 count, objIndex, slot, st->ssboHandle,
                                 st->cullCmdHandle[slot], pc.instanceVisibleHandle, prevVisible);
                }
                cmd->SetDrawDebugLabel("GBuffer InstancedMesh (逐实例剔除)");
                cmd->SetPushConstants(0, sizeof(pc), &pc);
                cmd->SetVertexBuffer(me->vertexBuffer, 0);
                cmd->SetIndexBuffer(me->indexBuffer);
                cmd->DrawIndexedIndirect(st->cullCmd[slot].get(), 0, 1,
                                         sizeof(InstanceIndirectCommand));
                st->visibleInstanceCount = prevVisible;
                continue;
            }

            cmd->SetDrawDebugLabel("GBuffer InstancedMesh");
            cmd->SetPushConstants(0, sizeof(pc), &pc);
            cmd->SetVertexBuffer(me->vertexBuffer, 0);
            cmd->SetIndexBuffer(me->indexBuffer);
            cmd->DrawIndexed(me->indexCount, count);
        }
    }

    cmd->EndOffscreenPass();
}

} // namespace he::render
