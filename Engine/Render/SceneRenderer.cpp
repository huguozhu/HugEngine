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
#include "Core/Log.h"
#include <mutex>

namespace he::render {

std::vector<DrawItem> SceneRenderer::Prepare(const FrameSceneSnapshot& snapshot,
                                             const CameraData& camera,
                                             rhi::IRHIBuffer* objectBuffer)
{
    std::vector<DrawItem> result;
    if (!objectBuffer) return result;

    const u32 total = static_cast<u32>(snapshot.draws.size());
    if (total == 0) return result;

    // ---- Step 1: 并行视锥剔除（世界 AABB 由收集侧算好，见 `CollectObjectItem`）----
    Frustum frustum = camera.GetFrustum();
    std::mutex mtx;
    std::vector<u32> visibleIdx;
    visibleIdx.reserve(total);

    if (enableFrustumCull) {
        JobSystem::Instance().ParallelForChunked(total, 64, [&](u32 start, u32 end) {
            std::vector<u32> local;
            local.reserve(end - start);
            for (u32 i = start; i < end; ++i) {
                const SnapshotDrawItem& item = snapshot.draws[i];
                // GPUObjectData 里的 AABB 是 float4（std430 布局），这里还原成 float3 做盒测试
                const he::AABB worldBounds{float3(item.object.boundsMin), float3(item.object.boundsMax)};
                if (!worldBounds.IsValid() || frustum.Intersects(worldBounds))
                    local.push_back(i);
            }
            if (!local.empty()) { std::lock_guard<std::mutex> lk(mtx); visibleIdx.insert(visibleIdx.end(), local.begin(), local.end()); }
        });
    } else {
        for (u32 i = 0; i < total; ++i)
            visibleIdx.push_back(i);
    }

    u32 visibleCount = static_cast<u32>(visibleIdx.size());
    if (visibleCount == 0) return result;
    if (visibleCount > MAX_OBJECTS) visibleCount = MAX_OBJECTS;

    // ---- Step 2: 上传 GPUObjectData + 构建 DrawList ----
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
        result.push_back(di);
    }
    objectBuffer->Unmap();

    static bool s_First = true;
    if (s_First) { HE_CORE_INFO("SceneRenderer: {} draws (from {} snapshot items)", visibleCount, total); s_First = false; }
    return result;
}

} // namespace he::render
