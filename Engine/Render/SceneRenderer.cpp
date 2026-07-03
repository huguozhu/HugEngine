#include "SceneRenderer.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/MeshComponent.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Threading/JobSystem.h"
#include "Core/Log.h"
#include <mutex>

namespace he::render {

std::vector<DrawItem> SceneRenderer::Prepare(he::World& world, he::SceneGraph& sg,
                                               const CameraData& camera,
                                               rhi::IRHIBuffer* objectBuffer)
{
    std::vector<DrawItem> result;
    if (!objectBuffer) return result;

    // ---- Step 1: 收集所有可绘制实体 + 预计算包围盒 ----
    struct Entry { he::MeshComponent* mesh; AABB worldBounds; float4x4 worldMatrix; };
    std::vector<Entry> entries;

    auto gather = [&](he::Entity e, he::MeshComponent& m) {
        if (m.GetIndexCount() == 0) return;
        float4x4 wm = sg.GetWorldMatrix(e);
        entries.push_back({&m, m.GetBounds().Transform(wm), wm});
    };
    world.ForEach<he::MeshComponent>([&](he::Entity e, he::MeshComponent& m) { gather(e, m); });
    world.ForEach<he::CubeComponent>([&](he::Entity e, he::CubeComponent& c) { gather(e, static_cast<he::MeshComponent&>(c)); });
    world.ForEach<he::SphereComponent>([&](he::Entity e, he::SphereComponent& s) { gather(e, static_cast<he::MeshComponent&>(s)); });

    u32 total = (u32)entries.size();
    if (total == 0) return result;

    // ---- Step 2: 并行视锥剔除 ----
    Frustum frustum = camera.GetFrustum();
    std::mutex mtx;
    std::vector<u32> visibleIdx; visibleIdx.reserve(total);

    JobSystem::Instance().ParallelForChunked(total, 64, [&](u32 start, u32 end) {
        std::vector<u32> local; local.reserve(end - start);
        for (u32 i = start; i < end; ++i)
            if (frustum.Intersects(entries[i].worldBounds))
                local.push_back(i);
        if (!local.empty()) { std::lock_guard<std::mutex> lk(mtx); visibleIdx.insert(visibleIdx.end(), local.begin(), local.end()); }
    });

    u32 visibleCount = (u32)visibleIdx.size();
    if (visibleCount == 0) return result;
    if (visibleCount > MAX_OBJECTS) visibleCount = MAX_OBJECTS;

    // ---- Step 3: 上传 GPUObjectData + 构建 DrawList ----
    auto* objData = static_cast<GPUObjectData*>(objectBuffer->Map());
    result.reserve(visibleCount);

    for (u32 vi = 0; vi < visibleCount; ++vi) {
        u32 ei = visibleIdx[vi];
        auto& e = entries[ei];

        // 材质数据
        PBRMaterial mat = GetDefaultMaterial();
        mat.baseColorFactor = e.mesh->baseColorFactor;
        mat.emissiveFactor  = e.mesh->emissiveFactor;
        mat.metallicFactor  = e.mesh->metallicFactor;
        mat.roughnessFactor = e.mesh->roughnessFactor;
        mat.aoFactor        = e.mesh->aoFactor;
        mat.alphaCutoff     = e.mesh->alphaCutoff;
        mat.alphaMode       = static_cast<AlphaMode>(e.mesh->alphaMode);
        mat.doubleSided     = e.mesh->doubleSided;
        mat.unlit           = e.mesh->unlit;

        GPUObjectData& obj = objData[vi];
        obj.worldMatrix = e.worldMatrix;
        FillObjectData(obj, mat);
        obj.materialID = e.mesh->materialID;

        result.push_back({e.mesh, vi});
    }
    objectBuffer->Unmap();

    static bool s_First = true;
    if (s_First) { HE_CORE_INFO("SceneRenderer: {} draws (from {} entities)", visibleCount, total); s_First = false; }
    return result;
}

} // namespace he::render
