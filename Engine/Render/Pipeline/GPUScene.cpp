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
#include "Core/Assert.h"   // HE_ASSERT
#include <cstring>
#include <algorithm>      // 【T7】std::min（合批对象数截断）

namespace he::render {

bool GPUScene::Initialize(rhi::IRHIDevice* device) {
    rhi::BufferDesc desc;
    desc.size  = sizeof(GPUSceneObject) * kMaxObjects;
    desc.usage = rhi::BufferUsage::Storage;
    desc.cpuAccess = true;
    m_Device     = device;                              // A-3：解析句柄用
    m_ObjectSSBO = device->CreateBufferHandle(desc);    // A-3
    HE_ASSERT(m_ObjectSSBO.IsValid(), "GPUScene: 对象 SSBO 句柄创建失败");

    m_Initialized = true;
    HE_CORE_INFO("GPUScene initialized (max {} objects, {} KB)", kMaxObjects,
        (sizeof(GPUSceneObject) * kMaxObjects) / 1024);
    return true;
}

void GPUScene::Shutdown() {
    // 【空保护】m_Device 可能为空（Shutdown 被调用两次，或 Initialize 失败后进入清理路径），
    // 直接解引用会崩溃（实测：02.Cube 退出时连续崩在多个 Shutdown 里）。
    if (!m_Device) return;

    m_Device->DestroyBufferHandle(m_ObjectSSBO);
    m_ObjectSSBO = {};
    m_Device     = nullptr;
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

void GPUScene::CollectBatchedFromSnapshot(const FrameSceneSnapshot& snapshot,
                                          const std::vector<u32>& batchSnapshotIndices) {
    m_DirtyIndices.clear();

    // 对象表长度 = 合批命令数（受**物体缓冲容量** `MAX_OBJECTS` 约束：绘制端按 objectID 索引
    // `GPUObjectData[MAX_OBJECTS]`，超出会让顶点着色器读越界）。不保留快照里的全部条目：
    //   · GPU 剔除只遍历 [0, batchCount)，越界条目无人读；
    //   · 长度与 `CollectFromSnapshot` 的口径不同 ⇒ 切回 CPU 模式时它会走"整表重建"分支，
    //     不会把"合批序号"误当成"快照序号"沿用（两类 objectID 的语义必须彻底分开）。
    const u32 batchCount = static_cast<u32>(
        std::min<size_t>(batchSnapshotIndices.size(), static_cast<size_t>(MAX_OBJECTS)));

    m_Objects.clear();
    m_CachedMatrices.clear();
    m_Objects.reserve(batchCount);
    m_CachedMatrices.reserve(batchCount);

    for (u32 i = 0; i < batchCount; ++i) {
        const u32 drawIndex = batchSnapshotIndices[i];
        if (drawIndex >= snapshot.draws.size()) {
            // 快照/合批不一致（理论上不应发生）：补一条空记录保持下标与命令一一对应，
            // 空的 AABB 会被视锥剔除掉，宁可少画也不读越界内存。
            GPUSceneObject empty{};
            empty.objectID = i;
            empty.visibilityFlags = 0;
            m_Objects.push_back(empty);
            m_CachedMatrices.push_back(float4x4(1.0f));
            continue;
        }
        const SnapshotDrawItem& item = snapshot.draws[drawIndex];
        GPUSceneObject o = MakeObjectRecord(item);
        // 【关键】objectID = 合批序号 = 间接命令 firstInstance = 物体缓冲下标
        o.objectID = i;
        // 实例化网格由逐实例路径绘制，不进 GPU 驱动的间接绘制
        if (item.bInstanced) o.visibilityFlags = 0;
        // 绘制参数（indexCount/firstIndex/vertexOffset）由 MeshBatcher::FillGPUScene 随后按同序写入
        m_Objects.push_back(o);
        m_CachedMatrices.push_back(item.object.worldMatrix);
    }

    // 顺序与 objectID 每帧都可能变化（收集集增删），全量标脏由 `Upload` 一次写完
    for (u32 i = 0; i < batchCount; ++i) m_DirtyIndices.push_back(i);

    m_ObjectCount = batchCount;
}

void GPUScene::Upload(rhi::IRHIDevice* device) {
    if (m_DirtyIndices.empty()) return;
    auto* ssbo   = device->ResolveBuffer(m_ObjectSSBO);   // A-3：按句柄解析
    void* mapped = ssbo ? ssbo->Map() : nullptr;
    if (!mapped) return;
    u8* base=static_cast<u8*>(mapped);
    for (u32 idx : m_DirtyIndices)
        memcpy(base+idx*sizeof(GPUSceneObject), &m_Objects[idx], sizeof(GPUSceneObject));
    if (auto* ssbo = device->ResolveBuffer(m_ObjectSSBO)) ssbo->Unmap();
}

} // namespace he::render
