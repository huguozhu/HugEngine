// Pipeline/GPUScene.cpp — GPU 场景数据管线实现
#include "Pipeline/GPUScene.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
// 阶段 1 T1.3b-3：收集口径集中到快照构造器（本文件不再自己遍历 ECS 组件）
#include "Threading/SceneSnapshotBuilder.h"
#include "Scene/MeshComponent.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/BillboardComponent.h"
#include "Scene/TextRenderComponent.h"
#include "Scene/DecalComponent.h"
#include "Scene/InstancedMeshComponent.h"
#include "Scene/SkeletalMeshComponent.h"
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
    o.localToWorld = wm;
    o.boundsMin=float4(b.min,0);
    o.boundsMax=float4(b.max,0);
    o.objectID=idx;
    o.visibilityFlags=1;
    o.meshIndex=0;
    o.indexCount=0;
    o.firstIndex=0;
    o.vertexOffset=0;
    // 由 MeshBatcher 填充;
}

// 阶段 1 第③段：`Collect(world, sg, camera)` 过渡重载已删除 —— 调用方直接消费自己的快照。
// 收集口径的唯一实现仍在 `SceneSnapshotBuilder::BuildObjects`。

void GPUScene::CollectFromSnapshot(const FrameSceneSnapshot& snapshot) {
    m_DirtyIndices.clear();
    const u32 count = static_cast<u32>(snapshot.draws.size());

    if (m_Objects.size() != count) {
        // 收集集合变化（物体增删/首次收集）：整表重建并全部标脏
        m_Objects.clear();
        m_CachedMatrices.clear();
        m_Objects.reserve(count);
        m_CachedMatrices.reserve(count);
        for (u32 i = 0; i < count; ++i) {
            m_Objects.push_back(MakeObjectRecord(snapshot.draws[i]));
            m_CachedMatrices.push_back(snapshot.draws[i].object.worldMatrix);
            m_DirtyIndices.push_back(i);
        }
    } else {
        // 集合不变：只更新世界矩阵变化的条目（与旧增量分支同一口径：广告牌每帧随相机变化，
        // 天然每帧 dirty）
        for (u32 i = 0; i < count; ++i) {
            const float4x4& wm = snapshot.draws[i].object.worldMatrix;
            if (wm != m_CachedMatrices[i]) {
                m_Objects[i] = MakeObjectRecord(snapshot.draws[i]);
                m_CachedMatrices[i] = wm;
                m_DirtyIndices.push_back(i);
            }
        }
    }
    m_ObjectCount = static_cast<u32>(m_Objects.size());
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
