// Pipeline/GPUScene.cpp — GPU 场景数据管线实现
#include "Pipeline/GPUScene.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/MeshComponent.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/BillboardComponent.h"
#include "Scene/TextRenderComponent.h"
#include "Scene/DecalComponent.h"
#include "Scene/InstancedMeshComponent.h"
#include "Core/Log.h"
#include <cstring>

namespace he::render {

bool GPUScene::Initialize(rhi::IRHIDevice* device) {
    rhi::BufferDesc desc;
    desc.size  = sizeof(GPUSceneObject) * kMaxObjects;
    desc.usage = rhi::BufferUsage::Storage;
    desc.cpuAccess = true;
    m_ObjectSSBO = device->CreateBuffer(desc);

    m_Initialized = true;
    HE_CORE_INFO("GPUScene initialized (max {} objects, {} KB)", kMaxObjects,
        (sizeof(GPUSceneObject) * kMaxObjects) / 1024);
    return true;
}

void GPUScene::Shutdown() {
    m_ObjectSSBO.reset();
    m_Objects.clear();
    m_ObjectCount = 0;
    m_Initialized = false;
}

static void FillObj(GPUSceneObject& o, const float4x4& wm, const AABB& b, u32 idx) {
    o.localToWorld = wm; o.boundsMin=float4(b.min,0); o.boundsMax=float4(b.max,0);
    o.objectID=idx; o.visibilityFlags=1; o.meshIndex=0;
    o.indexCount=0; o.firstIndex=0; o.vertexOffset=0;  // 由 MeshBatcher 填充
}

void GPUScene::Collect(World& world, SceneGraph& sg, const CameraData& camera) {
    m_DirtyIndices.clear();

    if (m_Objects.empty()) {
        // 首次：全量收集
        auto collect = [&](Entity e, auto& comp, const float4x4& wm, u32 matID) {
            if (comp.GetIndexCount()==0) return;
            GPUSceneObject o{};
            FillObj(o,wm,comp.GetBounds().Transform(wm),(u32)m_Objects.size());
            o.materialIndex=matID; m_Objects.push_back(o);
            m_CachedMatrices.push_back(wm);
            m_DirtyIndices.push_back((u32)m_Objects.size()-1);
        };
        world.ForEach<MeshComponent>([&](Entity e, MeshComponent& mc){collect(e,mc,sg.GetWorldMatrix(e),mc.materialID);});
        world.ForEach<CubeComponent>([&](Entity e, CubeComponent& cc){collect(e,cc,sg.GetWorldMatrix(e),0);});
        world.ForEach<SphereComponent>([&](Entity e, SphereComponent& sc){collect(e,sc,sg.GetWorldMatrix(e),0);});
        // 广告牌/3D 文字：矩阵对齐相机（每帧随相机变化，天然每帧 dirty）
        auto collectBillboard = [&](Entity e, BillboardComponent& bb) {
            float4x4 base = sg.GetWorldMatrix(e);
            float4x4 wm = BillboardComponent::MakeBillboardMatrix(
                float3(base[3]), camera.forward, camera.up, bb.size);
            collect(e, bb, wm, bb.materialID);
        };
        world.ForEach<BillboardComponent>([&](Entity e, BillboardComponent& bb){ collectBillboard(e, bb); });
        world.ForEach<TextRenderComponent>([&](Entity e, TextRenderComponent& tr){ collectBillboard(e, tr); });
        world.ForEach<DecalComponent>([&](Entity e, DecalComponent& dc){ collect(e, dc, sg.GetWorldMatrix(e), dc.materialID); });
        world.ForEach<InstancedMeshComponent>([&](Entity e, InstancedMeshComponent& im){ collect(e, im, sg.GetWorldMatrix(e), im.materialID); });
    } else {
        // 增量：只更新变化的对象
        u32 idx=0;
        auto update = [&](Entity e, auto& comp, const float4x4& wm) {
            if (comp.GetIndexCount()==0){idx++;return;}
            if (idx>=m_Objects.size()) return;
            if (wm!=m_CachedMatrices[idx]) {
                FillObj(m_Objects[idx],wm,comp.GetBounds().Transform(wm),idx);
                m_CachedMatrices[idx]=wm; m_DirtyIndices.push_back(idx);
            }
            idx++;
        };
        world.ForEach<MeshComponent>([&](Entity e, MeshComponent& mc){update(e,mc,sg.GetWorldMatrix(e));});
        world.ForEach<CubeComponent>([&](Entity e, CubeComponent& cc){update(e,cc,sg.GetWorldMatrix(e));});
        world.ForEach<SphereComponent>([&](Entity e, SphereComponent& sc){update(e,sc,sg.GetWorldMatrix(e));});
        auto updateBillboard = [&](Entity e, BillboardComponent& bb) {
            float4x4 base = sg.GetWorldMatrix(e);
            update(e, bb, BillboardComponent::MakeBillboardMatrix(
                float3(base[3]), camera.forward, camera.up, bb.size));
        };
        world.ForEach<BillboardComponent>([&](Entity e, BillboardComponent& bb){ updateBillboard(e, bb); });
        world.ForEach<TextRenderComponent>([&](Entity e, TextRenderComponent& tr){ updateBillboard(e, tr); });
        world.ForEach<DecalComponent>([&](Entity e, DecalComponent& dc){ update(e, dc, sg.GetWorldMatrix(e)); });
        world.ForEach<InstancedMeshComponent>([&](Entity e, InstancedMeshComponent& im){ update(e, im, sg.GetWorldMatrix(e)); });
    }
    m_ObjectCount=(u32)m_Objects.size();
}

void GPUScene::Upload(rhi::IRHIDevice* device) {
    if (m_DirtyIndices.empty()) return;
    void* mapped=m_ObjectSSBO->Map();
    if (!mapped) return;
    u8* base=static_cast<u8*>(mapped);
    for (u32 idx : m_DirtyIndices)
        memcpy(base+idx*sizeof(GPUSceneObject), &m_Objects[idx], sizeof(GPUSceneObject));
    m_ObjectSSBO->Unmap();
}

} // namespace he::render
