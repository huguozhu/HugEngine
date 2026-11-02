// SceneRenderer.cpp — 通用几何体数据准备器（阶段 1 §15.1 第②段：只消费快照）
//
// 【这次改动做了什么】收集（遍历 ECS、算世界矩阵/世界 AABB/材质）已经在游戏线程由
// `SceneSnapshotBuilder::BuildObjects` 做完；本类只做两件事：**视锥剔除**与
// **把条目的 GPUObjectData 整块上传**。于是：
//   · `Prepare` 的签名去掉了 `World&` / `SceneGraph&`（附录 B1 因此下降）；
//   · `DrawItem::mesh` 这个组件指针字段被删除（附录 E 的组件指针闸门收敛点）——
//     消费侧改为按 `meshIndex` 查 `MeshRegistry` 取顶点/索引缓冲。
// 语义保持不变：剔除口径（世界 AABB + 并行分块 + `MAX_OBJECTS` 截断）、可见顺序、
// objectIndex 的分配方式（= 可见列表下标）都与旧实现逐条一致。
#include "SceneRenderer.h"
#include "Math/Geometry.h"        // he::AABB（从快照的 world AABB 还原）+ Frustum
#include "Threading/JobSystem.h"
#include "Threading/ParallelCollect.h"   // 确定性紧凑化（并行标记 + 分块前缀和）
#include "Threading/CpuSections.h"       // HE_CPU_PASSES：剔除/上传两段的读数
#include "Core/Log.h"
// `#include <mutex>` 已移除：确定性写回后不再需要锁（见 Step 1 的说明）

namespace he::render {

std::vector<DrawItem> SceneRenderer::Prepare(const FrameSceneSnapshot& snapshot,
                                             const CameraData& camera,
                                             rhi::IRHIBuffer* objectBuffer)
{
    std::vector<DrawItem> result;
    if (!objectBuffer) return result;

    const u32 total = static_cast<u32>(snapshot.draws.size());
    if (total == 0) return result;

    // ---- Step 1: 并行视锥剔除 + **确定性**紧凑化（世界 AABB 由收集侧算好，见 `CollectObjectItem`）----
    // 【为什么要确定性】旧写法是"加锁把各 chunk 的本地向量 append 到全局"，输出顺序 = 线程完成序
    // ⇒ 同一输入两次运行的可见列表顺序都可能不同：下游 TAA / 时域降噪的历史对应被打乱、A/B 读数
    // 不可重复、MTCR 分块的内容也随之漂移。现改成两步：并行标记（无锁）→ 按下标升序紧凑化。
    Frustum frustum = camera.GetFrustum();
    std::vector<u8> hit(total, 0u);
    std::vector<u32> visibleIdx(total);
    u32 visibleCount = 0;

    {
        CpuScope cullScope(CpuSection::Cull);   // 【读数】剔除段 = 标记 + 确定性紧凑化
        if (enableFrustumCull) {
            JobSystem::Instance().ParallelForChunked(total, kCollectChunkSize, [&](u32 start, u32 end) {
                for (u32 i = start; i < end; ++i) {
                    const SnapshotDrawItem& item = snapshot.draws[i];
                    // GPUObjectData 里的 AABB 是 float4（std430 布局），这里还原成 float3 做盒测试
                    const he::AABB worldBounds{float3(item.object.boundsMin), float3(item.object.boundsMax)};
                    if (!worldBounds.IsValid() || frustum.Intersects(worldBounds)) hit[i] = 1u;
                }
            });
        } else {
            std::fill(hit.begin(), hit.end(), static_cast<u8>(1u));
        }
        visibleCount = CompactHits(hit, visibleIdx);
    }

    if (visibleCount == 0) return result;
    if (visibleCount > MAX_OBJECTS) visibleCount = MAX_OBJECTS;

    // ---- Step 2: 上传 GPUObjectData + 构建 DrawList ----
    // 【读数】上传段：Map + 逐条写回 + Unmap（多线程优化方案 §2.3 的目标段）
    CpuScope uploadScope(CpuSection::Upload);
    auto* objData = static_cast<GPUObjectData*>(objectBuffer->Map());
    if (!objData) return result;      // 映射失败：返回空列表（调用方跳过绘制），不写野指针
    result.reserve(visibleCount);

    for (u32 vi = 0; vi < visibleCount; ++vi) {
        const SnapshotDrawItem& item = snapshot.draws[visibleIdx[vi]];

        // 【整块拷贝】材质参数（E-3：收集侧用 `MakePBRMaterial` + `FillObjectData` 算好）、
        // 世界矩阵、世界 AABB、materialID 都在 `item.object` 里 —— 消费侧不再重算，
        // 从根上避免"两边各算一份然后漂移"。
        objData[vi] = item.object;

        DrawItem di{};
        di.objectIndex = vi;
        di.bInstanced  = item.bInstanced;
        di.meshIndex   = item.meshIndex;   // 顶点/索引缓冲由消费侧按它查注册表
        di.materialID  = item.object.materialID;   // 非 bindless 后端逐次绘制绑定材质纹理用
        result.push_back(di);
    }
    objectBuffer->Unmap();

    static bool s_First = true;
    if (s_First) { HE_CORE_INFO("SceneRenderer: {} draws (from {} snapshot items)", visibleCount, total); s_First = false; }
    return result;
}

// ============================================================================
// 【A4】PrepareBatched —— 按合批顺序准备物体数据（GPU 驱动的间接绘制路径）
// ============================================================================
std::vector<DrawItem> SceneRenderer::PrepareBatched(const FrameSceneSnapshot& snapshot,
                                                    const CameraData& camera,
                                                    rhi::IRHIBuffer* objectBuffer,
                                                    const std::vector<u32>& batchSnapshotIndices)
{
    std::vector<DrawItem> result;
    if (!objectBuffer || batchSnapshotIndices.empty()) return result;

    // 合批条目数受物体缓冲容量约束（绘制端按 objectID 索引 GPUObjectData[MAX_OBJECTS]）
    const u32 batchCount = static_cast<u32>(
        std::min<size_t>(batchSnapshotIndices.size(), static_cast<size_t>(MAX_OBJECTS)));
    if (batchCount == 0) return result;

    CpuScope cullScope(CpuSection::Cull);   // 【读数】剔除段（与 Prepare 同一段名，便于横向对比）
    Frustum frustum = camera.GetFrustum();
    auto passesCull = [&](const SnapshotDrawItem& item) {
        if (!enableFrustumCull) return true;
        const he::AABB worldBounds{float3(item.object.boundsMin), float3(item.object.boundsMax)};
        // 与 Prepare 完全同一判据：包围盒无效（不存在）时视为可见，交给 GPU 再判
        return !worldBounds.IsValid() || frustum.Intersects(worldBounds);
    };
    // 剔除结果整表先算好（合批区间不能紧凑化，只能逐条打标记）
    std::vector<u8> visible(snapshot.draws.size(), 1u);
    for (u32 i = 0; i < static_cast<u32>(snapshot.draws.size()); ++i)
        visible[i] = passesCull(snapshot.draws[i]) ? 1u : 0u;

    CpuScope uploadScope(CpuSection::Upload);   // 【读数】上传段
    auto* objData = static_cast<GPUObjectData*>(objectBuffer->Map());
    if (!objData) {
        // 映射失败：不写野指针，返回空列表让调用方回退逐 draw
        HE_CORE_ERROR("SceneRenderer::PrepareBatched: 物体缓冲映射失败（{} 个合批物体）", batchCount);
        return result;
    }

    result.reserve(batchCount);
    // 合批覆盖了哪些快照条目（追加 Spline/Skeletal 时要去重）
    std::vector<u8> inBatch(snapshot.draws.size(), 0u);

    // ── ① 合批区间：下标 = 合批序号，**不紧凑化** ──
    for (u32 i = 0; i < batchCount; ++i) {
        const u32 drawIndex = batchSnapshotIndices[i];
        DrawItem di{};
        di.objectIndex = i;   // 下标 = 合批序号 = 间接命令 firstInstance
        if (drawIndex < static_cast<u32>(snapshot.draws.size())) {
            const SnapshotDrawItem& item = snapshot.draws[drawIndex];
            // 整块拷贝：世界矩阵 / 世界 AABB / 材质参数已由收集侧算好，与 CPU 路径同源
            objData[i] = item.object;
            di.bInstanced = item.bInstanced;
            di.meshIndex  = item.meshIndex;
            // 非 bindless 后端（D3D11）逐次绘制要用的材质纹理基址：与 CPU 路径同源
            di.materialID = item.object.materialID;
            di.bCPUCulled = (visible[drawIndex] == 0u);
            inBatch[drawIndex] = 1u;
        } else {
            // 快照 / 合批不一致（理论上不应发生）：写零矩阵让它退化，并标记为不可见
            objData[i]   = GPUObjectData{};
            di.meshIndex = 0;
            di.bCPUCulled = true;
        }
        result.push_back(di);
    }

    // ── ② 追加合批收集集之外的条目（Spline / Skeletal：不并入合批）──
    // 它们的几何仍由各自的顶点/索引缓冲提供（逐 draw 循环 / 蒙皮 Pass），但 `objectIndex`
    // 必须指向物体缓冲里**真正写入过**的槽位 ⇒ 接在合批区间之后顺序分配。
    // 只有 CPU 可见的才上传（GPU 剔除不遍历这些条目，没人会画它们）。
    u32 nextSlot = batchCount;
    for (u32 drawIndex = 0; drawIndex < static_cast<u32>(snapshot.draws.size()); ++drawIndex) {
        const SnapshotDrawItem& item = snapshot.draws[drawIndex];
        if (inBatch[drawIndex]) continue;
        const bool batchedClass = (item.meshClass != SnapshotMeshClass::Spline &&
                                   item.meshClass != SnapshotMeshClass::Skeletal);
        if (batchedClass) continue;          // 其他类别都已在合批区间里
        if (visible[drawIndex] == 0u) continue;   // 不可见：不上传（也避免脏槽位被引用）
        if (nextSlot >= MAX_OBJECTS) break;  // 容量上限：宁可少画，不越界写
        objData[nextSlot] = item.object;
        DrawItem di{};
        di.objectIndex = nextSlot;
        di.bInstanced  = item.bInstanced;
        di.meshIndex   = item.meshIndex;
        // 非 bindless 后端（D3D11）逐次绘制要用的材质纹理基址：与 CPU 路径同源
        di.materialID  = item.object.materialID;
        di.bCPUCulled  = false;
        result.push_back(di);
        ++nextSlot;
    }

    objectBuffer->Unmap();

    static bool s_First = true;
    if (s_First) {
        HE_CORE_INFO("SceneRenderer::PrepareBatched: {} 条合批物体 + {} 条非合批物体（快照 {} 条）",
                     batchCount, nextSlot - batchCount, (u32)snapshot.draws.size());
        s_First = false;
    }
    return result;
}

} // namespace he::render
