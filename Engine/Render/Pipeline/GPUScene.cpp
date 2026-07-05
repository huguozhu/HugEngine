// Pipeline/GPUScene.cpp — GPU 场景数据管线实现
#include "Pipeline/GPUScene.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/MeshComponent.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
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

void GPUScene::Collect(World& world, SceneGraph& sg) {
    m_Objects.clear();

    // 收集 MeshComponent
    world.ForEach<MeshComponent>([&](Entity e, MeshComponent& mc) {
        if (mc.GetIndexCount() == 0) return;
        GPUSceneObject obj{};
        obj.localToWorld = sg.GetWorldMatrix(e);
        const auto& bb = mc.GetBounds();
        AABB wb = bb.Transform(obj.localToWorld);
        obj.boundsMin  = float4(wb.min, 0);
        obj.boundsMax  = float4(wb.max, 0);
        obj.meshIndex  = 0;  // Phase 2 填充
        obj.materialIndex = mc.materialID;
        obj.objectID   = (u32)m_Objects.size();
        obj.visibilityFlags = 1;  // visible
        m_Objects.push_back(obj);
    });

    // 收集 CubeComponent
    world.ForEach<CubeComponent>([&](Entity e, CubeComponent& cc) {
        if (cc.GetIndexCount() == 0) return;
        GPUSceneObject obj{};
        obj.localToWorld = sg.GetWorldMatrix(e);
        const auto& ccb = cc.GetBounds(); AABB cwb = ccb.Transform(obj.localToWorld);
        obj.boundsMin  = float4(cwb.min, 0);
        obj.boundsMax  = float4(cwb.max, 0);
        obj.meshIndex  = 0;
        obj.materialIndex = 0;
        obj.objectID   = (u32)m_Objects.size();
        obj.visibilityFlags = 1;
        m_Objects.push_back(obj);
    });

    // 收集 SphereComponent
    world.ForEach<SphereComponent>([&](Entity e, SphereComponent& sc) {
        if (sc.GetIndexCount() == 0) return;
        GPUSceneObject obj{};
        obj.localToWorld = sg.GetWorldMatrix(e);
        const auto& scb = sc.GetBounds(); AABB swb = scb.Transform(obj.localToWorld);
        obj.boundsMin  = float4(swb.min, 0);
        obj.boundsMax  = float4(swb.max, 0);
        obj.meshIndex  = 0;
        obj.materialIndex = 0;
        obj.objectID   = (u32)m_Objects.size();
        obj.visibilityFlags = 1;
        m_Objects.push_back(obj);
    });

    m_ObjectCount = (u32)m_Objects.size();
}

void GPUScene::Upload(rhi::IRHIDevice* device) {
    if (m_Objects.empty()) return;

    usize size = m_Objects.size() * sizeof(GPUSceneObject);
    void* mapped = m_ObjectSSBO->Map();
    if (mapped) {
        memcpy(mapped, m_Objects.data(), size);
        m_ObjectSSBO->Unmap();
    }
}

} // namespace he::render
