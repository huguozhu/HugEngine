#include "SceneRenderer.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/MeshComponent.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/BillboardComponent.h"
#include "Scene/TextRenderComponent.h"
#include "Scene/DecalComponent.h"
#include "Scene/InstancedMeshComponent.h"
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
    struct Entry { he::MeshComponent* mesh; AABB worldBounds; float4x4 worldMatrix; bool bInstanced = false; };
    std::vector<Entry> entries;

    auto gather = [&](he::Entity e, he::MeshComponent& m) {
        if (m.GetIndexCount() == 0) return;
        float4x4 wm = sg.GetWorldMatrix(e);
        entries.push_back({&m, m.GetBounds().Transform(wm), wm, false});
    };
    world.ForEach<he::MeshComponent>([&](he::Entity e, he::MeshComponent& m) { gather(e, m); });
    world.ForEach<he::CubeComponent>([&](he::Entity e, he::CubeComponent& c) { gather(e, static_cast<he::MeshComponent&>(c)); });
    world.ForEach<he::SphereComponent>([&](he::Entity e, he::SphereComponent& s) { gather(e, static_cast<he::MeshComponent&>(s)); });
    // 广告牌：世界矩阵替换为对齐相机的 billboard 矩阵（每帧随相机旋转）
    auto gatherBillboard = [&](he::Entity e, he::BillboardComponent& b) {
        if (b.GetIndexCount() == 0) return;
        // 位置取世界矩阵平移分量（支持挂在父节点下；旋转/缩放忽略，MVP）
        float4x4 base = sg.GetWorldMatrix(e);
        float4x4 wm = he::BillboardComponent::MakeBillboardMatrix(
            float3(base[3]), camera.forward, camera.up, b.size);
        entries.push_back({static_cast<he::MeshComponent*>(&b), b.GetBounds().Transform(wm), wm});
    };
    world.ForEach<he::BillboardComponent>([&](he::Entity e, he::BillboardComponent& b) { gatherBillboard(e, b); });
    // 3D 文字（继承 Billboard，同样对齐相机）
    world.ForEach<he::TextRenderComponent>([&](he::Entity e, he::TextRenderComponent& t) { gatherBillboard(e, t); });
    // 贴花：固定朝向（Transform 摆放），走普通 mesh 路径
    world.ForEach<he::DecalComponent>([&](he::Entity e, he::DecalComponent& d) { gather(e, d); });
    // 实例化网格（B1）：登记一个对象条目（材质数据用），实例由专用 Pass 绘制
    world.ForEach<he::InstancedMeshComponent>([&](he::Entity e, he::InstancedMeshComponent& im) {
        if (im.GetIndexCount() == 0) return;
        float4x4 wm = sg.GetWorldMatrix(e);
        entries.push_back({static_cast<he::MeshComponent*>(&im),
                           im.GetBounds().Transform(wm), wm, true});
    });

    u32 total = (u32)entries.size();
    if (total == 0) return result;

    // ---- Step 2: 并行视锥剔除 ----
    Frustum frustum = camera.GetFrustum();
    std::mutex mtx;
    std::vector<u32> visibleIdx; visibleIdx.reserve(total);

    if (enableFrustumCull) {
        JobSystem::Instance().ParallelForChunked(total, 64, [&](u32 start, u32 end) {
            std::vector<u32> local; local.reserve(end - start);
            for (u32 i = start; i < end; ++i) {
                if (!entries[i].worldBounds.IsValid() || frustum.Intersects(entries[i].worldBounds))
                    local.push_back(i);
            }
            if (!local.empty()) { std::lock_guard<std::mutex> lk(mtx); visibleIdx.insert(visibleIdx.end(), local.begin(), local.end()); }
        });
    } else {
        for (u32 i = 0; i < total; ++i)
            visibleIdx.push_back(i);
    }

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
        // 纹理路径 → textureMask（无纹理槽 shader 不采样，避免占位纹理污染）
        mat.baseColorTexture         = e.mesh->baseColorTexture;
        mat.normalTexture            = e.mesh->normalTexture;
        mat.metallicRoughnessTexture = e.mesh->metallicRoughnessTexture;
        mat.occlusionTexture         = e.mesh->occlusionTexture;
        mat.emissiveTexture          = e.mesh->emissiveTexture;

        GPUObjectData& obj = objData[vi];
        obj.worldMatrix = e.worldMatrix;
        FillObjectData(obj, mat);
        obj.materialID = e.mesh->materialID;

        result.push_back({e.mesh, vi, e.bInstanced});
    }
    objectBuffer->Unmap();

    static bool s_First = true;
    if (s_First) { HE_CORE_INFO("SceneRenderer: {} draws (from {} entities)", visibleCount, total); s_First = false; }
    return result;
}

} // namespace he::render
