// Pipeline/GBufferRenderer_GPU.cpp — GPU Driven GBuffer 渲染
// DrawIndexedIndirectCount（绘制条数由 GPU 剔除写出的计数缓冲决定，CPU 不参与）
// MeshBatcher::Build + GPUScene 按合批顺序重建 + FillGPUScene 在 DeferredPipeline::BuildFrameGraph 中完成
#include "Pipeline/GBufferRenderer_GPU.h"
#include "Pipeline/MeshBatcher.h"
#include "Pipeline/InstanceCuller.h"   // 任务 25：逐实例剔除（状态表也在它这里）
#include "Threading/MeshRegistry.h"    // 阶段 1 第①段：按 meshIndex 取顶点/索引缓冲
#include "Scene/MeshComponent.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Core/Log.h"
#include <cstdio>
#include <cstdlib>   // std::getenv（GPU 路径回退开关）
#include <algorithm>

namespace he::render {

namespace {

/// 【T7】GPU 驱动路径的物体数据准备：按**合批顺序**把全部物体写进 GPUObjectData[]，
/// 并返回同序的 DrawItem 列表（实例化绘制仍按 meshIndex 从这里查对象条目）。
///
/// 【为什么不能再用 `SceneRenderer::Prepare`】那个函数做的是 CPU 视锥裁剪，只把**可见子集**
/// 按自己的顺序写进物体缓冲；而 GPU 剔除发出的间接命令用 `firstInstance = 合批序号` 索引物体
/// 缓冲（`GBuffer.vert.slang` 的 `useInstanceID == 1` 分支）。两边顺序/长度不同 ⇒ 画到别的
/// 物体（T7 记录的三处问题之一：物体缓冲顺序与 GPU 场景不一致）。
///
/// 【下标一致性（T7 的核心契约）】以下四处必须是同一个序号：
///   合批序号 = GPUScene 对象下标（CollectBatchedFromSnapshot）= 间接命令 firstInstance
///            = 物体缓冲下标（本函数写入的位置）
///
/// @return 与合批命令同序的绘制项；`objectBuffer` 为空/映射失败时返回空列表（调用方回退）
std::vector<DrawItem> BuildBatchDrawItemsAndUpload(GBufferContext& ctx,
                                                   const FrameSceneSnapshot& snapshot) {
    std::vector<DrawItem> items;
    const MeshBatcher* mb = ctx.meshBatcher;
    if (!mb || !ctx.objectBuffer) return items;

    const auto& batchIndices = mb->GetBatchSnapshotIndices();
    const u32 count = static_cast<u32>(
        std::min<size_t>(batchIndices.size(), static_cast<size_t>(MAX_OBJECTS)));
    if (count == 0) return items;

    auto* objData = static_cast<GPUObjectData*>(ctx.objectBuffer->Map());
    if (!objData) {
        // 映射失败：不写野指针，返回空列表让调用方走 CPU 回退
        HE_CORE_ERROR("GBuffer_GPU: 物体缓冲映射失败（{} 个合批物体）", count);
        return items;
    }

    items.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        const u32 drawIndex = batchIndices[i];
        DrawItem di{};
        di.objectIndex = i;   // 下标 = 合批序号（间接命令 firstInstance）
        if (drawIndex < static_cast<u32>(snapshot.draws.size())) {
            const SnapshotDrawItem& item = snapshot.draws[drawIndex];
            // 整块拷贝：世界矩阵/世界 AABB/材质参数已由收集侧算好，与 CPU 路径**同源**
            objData[i]    = item.object;
            di.bInstanced = item.bInstanced;
            di.meshIndex  = item.meshIndex;
        } else {
            // 快照/合批不一致（理论上不应发生）：写零矩阵让它退化，避免读越界内存
            objData[i] = GPUObjectData{};
            di.meshIndex = 0;
        }
        items.push_back(di);
    }
    ctx.objectBuffer->Unmap();
    return items;
}

} // namespace

bool GBufferRenderer_GPU::Initialize(GBufferContext& ctx) {
    (void)ctx;
    return true;
}

void GBufferRenderer_GPU::Shutdown() {}

void GBufferRenderer_GPU::Render(rhi::IRHICommandList* cmd, GBufferContext& ctx,
                                  const FrameSceneSnapshot& snapshot,
                                  const CameraData& camera) {
    // ── 安全回退开关（T7）──
    // 默认走修好的 GPU 驱动路径；若现场出现异常（画面错乱/驱动兼容问题），设置环境变量
    // `HE_DISABLE_GPU_GBUFFER=1` 即可退回 CPU 逐物体绘制，无需重新编译。
    static const bool s_ForceCPUFallback = []() {
        const char* v = std::getenv("HE_DISABLE_GPU_GBUFFER");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();

    // GPU 驱动的判定条件（只依赖"能力/资源是否就绪"，**不**看 CPU 读回的可见数）：
    // 剔除开关 + 合批命令 + 间接缓冲 + 计数缓冲。绘制条数由 GPU 计数缓冲决定。
    const bool gpuDriven = ctx.gpuCulling != nullptr && ctx.gpuCulling->enabled
                        && !s_ForceCPUFallback
                        && ctx.meshBatcher != nullptr
                        && ctx.meshBatcher->GetCommandCount() > 0
                        && ctx.meshBatcher->GetVertexBuffer() != nullptr
                        && ctx.meshBatcher->GetIndexBuffer() != nullptr
                        && ctx.gpuCulling->GetIndirectBuffer() != nullptr
                        && ctx.gpuCulling->GetDrawCountBuffer() != nullptr
                        && ctx.objectBuffer != nullptr;

    u32 w = ctx.width, h = ctx.height;

    // 推送 bindless 纹理（Flush 自动遍历全部注册描述符集）
    ctx.device->GetBindlessHeap()->Flush();

    // 绑定 set=0
    ctx.device->UpdateDescriptorSet(ctx.descSet, rhi::kBindingObjectData, rhi::DescriptorType::StorageBuffer,
                                     ctx.objectBuffer);
    // 逐实例剔除必须在渲染通道外派发（Vulkan 限制）：先上传实例变换并剔除，通道内（CPU 回退段）只做间接绘制
    if (ctx.instanceCuller)
        ctx.instanceCuller->PrepareInstances(cmd, ctx.device, ctx.meshRegistry, snapshot,
                                             ctx.frameSlot, camera.GetViewProjMatrix());
    cmd->SetPipeline(ctx.pso);
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, ctx.descSet);

    // ── 物体数据：GPU 模式按合批顺序写全部条目；回退模式沿用 CPU 视锥裁剪版 Prepare ──
    std::vector<DrawItem> drawItems;
    if (gpuDriven) {
        drawItems = BuildBatchDrawItemsAndUpload(ctx, snapshot);
        // 物体缓冲写入失败（如 Map 失败）时不要拿脏数据做间接绘制：退回 CPU 准备路径
        if (drawItems.empty()) HE_CORE_ERROR("GBuffer_GPU: 合批物体写入失败，本帧退回 CPU 准备路径");
    } else {
        // CPU 回退：Prepare 会做视锥裁剪并按可见顺序重写物体缓冲（indexCount/材质同源）
        drawItems = ctx.sceneRenderer->Prepare(snapshot, camera, ctx.objectBuffer);
    }
    // 真正是否走 GPU 间接绘制：物体条目写成功才算
    const bool drawViaGPU = gpuDriven && !drawItems.empty();

    // 清除 + 开始 MRT（8 颜色 + 深度）
    rhi::ClearValue clears[9]{};
    clears[0].color[3] = 1.0f;
    clears[1].color[3] = 1.0f;
    clears[2].color[3] = 1.0f;
    clears[3].color[0] = 0.0f;
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
    cmd->SetFullscreenViewport(static_cast<u32>(w), static_cast<u32>(h));

    // GPU 模式 push constant 布局（192B）：viewProj / prevViewProj / objectIndex / useInstanceID
    //  + 两个实例句柄槽位（GPU 驱动路径填 0）。
    struct {
        float4x4 viewProjMatrix;
        float4x4 prevViewProjMatrix;
        u32      objectIndex;
        u32      useInstanceID;
        u32      instanceSSBOHandle;
        u32      instanceVisibleHandle;
        u32      _pad[12];
    } pc{};
    pc.viewProjMatrix     = camera.GetViewProjMatrix();
    pc.prevViewProjMatrix = ctx.prevViewProj;
    pc.objectIndex        = 0;
    pc.useInstanceID      = 1;   // 1 = GPU Driven：物体下标取 SV_InstanceID（= 间接命令 firstInstance）

    if (drawViaGPU) {
        // ── GPU 驱动绘制：顶点/索引缓冲 = MeshBatcher 的合并缓冲 ──
        // 间接命令里的 (firstIndex, indexCount) 指向合并缓冲，vertexOffset 恒 0（合批索引已是绝对索引）
        cmd->SetVertexBuffer(ctx.meshBatcher->GetVertexBuffer(), 0);
        cmd->SetIndexBuffer(ctx.meshBatcher->GetIndexBuffer(), 0);
        cmd->SetPushConstants(0, sizeof(pc), &pc);

        // ── DGC 路径 vs 传统 ExecuteIndirectCount 路径 ──
        if (ctx.dgc.enabled && ctx.dgc.sequenceBuffer && ctx.dgc.countBuffer) {
            // DGC 路径：由 GPU 生成 vkCmdDrawIndexed 命令（D3D12 侧等价为带计数缓冲的 ExecuteIndirect）
            rhi::IRHICommandList::DGCExecuteDesc dgcDesc{};
            dgcDesc.indirectCommandsLayout = ctx.dgc.indirectCommandsLayout;
            dgcDesc.indirectExecutionSet   = ctx.dgc.indirectExecutionSet;
            dgcDesc.sequencesBufferAddr    = ctx.dgc.sequenceBuffer->GetDeviceAddress();
            dgcDesc.maxSequenceCount       = ctx.dgc.maxSequenceCount;
            dgcDesc.sequenceCountAddr      = ctx.dgc.countBuffer->GetDeviceAddress();
            dgcDesc.preprocessBufferAddr   = ctx.dgc.preprocessBufferAddr;
            dgcDesc.preprocessBufferSize   = ctx.dgc.preprocessBufferSize;
            // 【T7】maxDrawCount 只是命令容量的上限（实际条数由 GPU 计数缓冲给出），
            // 不能再传 CPU 读回的可见数（那是上一帧的统计值）。
            dgcDesc.maxDrawCount           = GPUCulling::kMaxObjects;

            // DrawCall 调试 marker（RenderDoc 定位用）
            char label[64];
            snprintf(label, sizeof(label), "GBuffer DGC (max %u)", GPUCulling::kMaxObjects);
            cmd->SetDrawDebugLabel(label);
            cmd->ExecuteGeneratedCommands(dgcDesc);
        } else {
            // 传统路径：计数缓冲由 GPU 剔除写出，CPU 完全不参与"画几条"的决定
            char label[64];
            snprintf(label, sizeof(label), "GBuffer IndirectCount (max %u)", GPUCulling::kMaxObjects);
            cmd->SetDrawDebugLabel(label);
            cmd->DrawIndexedIndirectCount(ctx.gpuCulling->GetIndirectBuffer(), 0,
                                          ctx.gpuCulling->GetDrawCountBuffer(), 0,
                                          GPUCulling::kMaxObjects,
                                          sizeof(IndirectDrawCommand));
        }
    } else {
        // ── CPU 回退：逐对象直接绘制（objectIndex = 物体缓冲下标）──
        for (auto& di : drawItems) {
            // 任务 25：实例化网格由下面专门段落绘制
            if (di.bInstanced) continue;
            struct { float4x4 vp; float4x4 pvp; u32 oi; u32 uid; u32 inst; u32 vis; u32 _pad[12]; } cpc;
            cpc.vp   = pc.viewProjMatrix;
            cpc.pvp  = pc.prevViewProjMatrix;
            cpc.oi   = di.objectIndex;
            cpc.uid  = 0;
            cpc.inst = 0;
            cpc.vis  = 0;
            // DrawCall 调试 marker：标记当前绘制的物体（RenderDoc 定位用）
            char label[64];
            snprintf(label, sizeof(label), "GBuffer Obj#%u", di.objectIndex);
            cmd->SetDrawDebugLabel(label);
            cmd->SetPushConstants(0, sizeof(cpc), &cpc);
            // 顶点/索引缓冲按 `meshIndex` 从注册表取（第②段：`DrawItem` 不再持有组件指针）
            const MeshRegistryEntry* me = ctx.meshRegistry ? ctx.meshRegistry->Find(di.meshIndex) : nullptr;
            if (!me || !me->vertexBuffer || !me->indexBuffer) continue;
            cmd->SetVertexBuffer(me->vertexBuffer, 0);
            cmd->SetIndexBuffer(me->indexBuffer);
            cmd->DrawIndexed(me->indexCount);
        }
    }

    // ── 实例化网格（任务 25，与 CPU 模式/Forward 同一套机制）──
    // 【T7】两条路径都要执行：GPU 驱动的间接绘制只覆盖普通物体（实例化条目在 GPUScene 里
    // visibilityFlags = 0，被剔除着色器跳过），实例化网格仍走逐实例剔除 + 间接绘制。
    // 【阶段 1 第①段 / §15.1】数据来源改为**快照**（实例变换/开关/版本号按值带走），
    // 顶点/索引缓冲按 `meshIndex` 去注册表取，缓冲状态在 `InstanceCuller` 的实例状态表里。
    if (ctx.instanceCuller) {
        // 帧边界与逐实例剔除已在通道开始前由 PrepareInstances 完成
        const u32 slot = ctx.frameSlot % rhi::kMaxFramesInFlight;
        for (const SnapshotInstance& si : snapshot.instances) {
            if (si.transformCount == 0u) continue;
            // 变换切片越界保护：快照损坏时宁可少画，也不要读越界内存
            if (static_cast<usize>(si.transformOffset) + si.transformCount >
                snapshot.instanceTransforms.size()) continue;
            const MeshRegistryEntry* me = ctx.meshRegistry ? ctx.meshRegistry->Find(si.meshIndex) : nullptr;
            if (!me || !me->vertexBuffer || !me->indexBuffer) continue;

            u32 objIndex = 0;
            bool found = false;
            for (auto& di : drawItems) {
                // E-3②：按 meshIndex（整数）对齐对象条目 —— 组件地址比较已随组件指针一起退出
                if (si.meshIndex != 0u && si.meshIndex == di.meshIndex) {
                    objIndex = di.objectIndex; found = true; break;
                }
            }
            if (!found) continue;

            const u32 count = si.transformCount;
            const u32 instHandle = ctx.instanceCuller->UploadInstanceTransforms(
                ctx.device, si.meshIndex,
                snapshot.instanceTransforms.data() + si.transformOffset,
                count, si.transformVersion, si.sourceEntity);
            if (instHandle == 0) continue;
            InstanceCuller::InstanceState* st = ctx.instanceCuller->FindInstanceState(si.meshIndex);
            if (!st) continue;

            struct { float4x4 vp; float4x4 pvp; u32 oi; u32 uid; u32 inst; u32 vis; u32 _pad[12]; } ipc;
            ipc.vp   = pc.viewProjMatrix;
            ipc.pvp  = pc.prevViewProjMatrix;
            ipc.oi   = objIndex;
            ipc.uid  = 2;
            ipc.inst = instHandle;
            ipc.vis  = 0;

            // 剔除已在通道开始前派发（PrepareInstances）；未派发则整批绘制
            if (st->culledThisFrame) {
                ipc.vis = ctx.instanceCuller->GetVisibleIndicesHandle(slot);
                cmd->SetDrawDebugLabel("GBuffer InstancedMesh (逐实例剔除)");
                cmd->SetPushConstants(0, sizeof(ipc), &ipc);
                cmd->SetVertexBuffer(me->vertexBuffer, 0);
                cmd->SetIndexBuffer(me->indexBuffer);
                cmd->DrawIndexedIndirect(st->cullCmd[slot].get(), 0, 1,
                                         sizeof(InstanceIndirectCommand));
                continue;
            }

            cmd->SetDrawDebugLabel("GBuffer InstancedMesh");
            cmd->SetPushConstants(0, sizeof(ipc), &ipc);
            cmd->SetVertexBuffer(me->vertexBuffer, 0);
            cmd->SetIndexBuffer(me->indexBuffer);
            cmd->DrawIndexed(me->indexCount, count);
        }
    }

    cmd->EndOffscreenPass();

    // ── 统计日志（T7 可观测性）──
    // 说明：`GetLastVisibleCount()` 是**上一帧**读回的 GPU 计数缓冲值（CPU 同步读回），
    // 路径字段说明本帧确实走了间接绘制；首帧必打，之后每 120 帧打一次。
    static u32 s_LogFrame = 0;
    ++s_LogFrame;
    if (s_LogFrame <= 2u || s_LogFrame % 120u == 0u) {
        HE_CORE_INFO("GBuffer_GPU: 第 {} 帧 路径={} 合批={} 物体条目={} GPU可见(上帧读回)={}",
                     s_LogFrame,
                     drawViaGPU ? "GPU 间接绘制(DrawIndexedIndirectCount)" : "CPU 回退",
                     ctx.meshBatcher ? ctx.meshBatcher->GetCommandCount() : 0u,
                     static_cast<u32>(drawItems.size()),
                     ctx.gpuCulling ? ctx.gpuCulling->GetLastVisibleCount() : 0u);
    }
}

} // namespace he::render
