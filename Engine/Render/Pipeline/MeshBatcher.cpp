// Pipeline/MeshBatcher.cpp — Mesh 合并实现（阶段 1 §15.1 第③段第 4 批：改吃快照 + 注册表）
#include "Pipeline/MeshBatcher.h"
#include "Threading/MeshRegistry.h"   // 顶点/索引缓冲与顶点/索引数按 meshIndex 从这里取
#include "Pipeline/GPUScene.h"
#include "Pipeline/Material.h"   // 【任务 19】PBRMaterial / ComputeMaterialTextureMask（材质快照同源）
#include "RHI/RHI.h"
#include "Core/Log.h"

namespace he::render {

bool MeshBatcher::Build(const FrameSceneSnapshot& snapshot, const MeshRegistry& registry,
                        bool excludeDecals) {
    m_MergedVertices.clear();
    m_MergedIndices.clear();
    m_Commands.clear();
    m_DGCTokens.clear();
    m_MeshMaterials.clear();

    u32 baseVertex = 0;
    u32 baseIndex  = 0;

    // 收集一个快照条目（阶段 1 §15.1 第③段第 4 批：数据全部来自快照 + 注册表，不再遍历世界）
    auto collect = [&](const SnapshotDrawItem& item) {
        const MeshRegistryEntry* me = registry.Find(item.meshIndex);
        if (!me || !me->vertexBuffer || !me->indexBuffer) return;
        const u32 idxCount = me->indexCount;
        const u32 vtxCount = me->vertexCount;
        if (idxCount == 0 || vtxCount == 0) return;

        // Map 原始 VB，拷贝顶点
        void* vData = me->vertexBuffer->Map();
        if (vData) {
            u32 oldSize = (u32)m_MergedVertices.size();
            m_MergedVertices.resize(oldSize + vtxCount);
            memcpy(m_MergedVertices.data() + oldSize, vData, vtxCount * sizeof(StaticVertex));
            me->vertexBuffer->Unmap();
        }

        // Map 原始 IB，偏移后拷贝索引
        void* iData = me->indexBuffer->Map();
        if (iData) {
            u32 oldSize = (u32)m_MergedIndices.size();
            m_MergedIndices.resize(oldSize + idxCount);
            auto* srcIndices = static_cast<u32*>(iData);
            for (u32 i = 0; i < idxCount; ++i)
                m_MergedIndices[oldSize + i] = srcIndices[i] + baseVertex;  // 偏移顶点索引
            me->indexBuffer->Unmap();
        }

        // 记录间接绘制命令
        m_Commands.push_back({idxCount, 1, baseIndex, (i32)baseVertex, 0});

        // 【§14.8 任务 19】材质快照：与上面那条命令**同一个 collect 调用**里产出 ⇒ 顺序天然对齐。
        // 字段来源与收集侧填 `GPUObjectData` 时同一批（`MakePBRMaterial` + `FillObjectData`）：
        // 因子与纹理掩码直接取快照条目里的 `object` ⇒ 与 GBuffer 路径**同源**，不会各自漂移。
        {
            MergedMeshMaterial material{};
            material.baseColorFactor[0] = item.object.baseColorFactor.x;
            material.baseColorFactor[1] = item.object.baseColorFactor.y;
            material.baseColorFactor[2] = item.object.baseColorFactor.z;
            material.baseColorFactor[3] = item.object.baseColorFactor.w;
            material.metallicFactor     = item.object.metallicFactor;
            material.roughnessFactor    = item.object.roughnessFactor;
            material.textureMask        = item.object.textureMask;
            material.bindlessTextureBase = item.object.materialID;
            m_MeshMaterials.push_back(material);
        }

        // DGC 模式：记录含 objectIndex 的 draw token
        // objectIndex = 当前物体在 GPUScene 中的索引（与 m_Commands 顺序一致）
        DGCDrawToken dgcToken;
        dgcToken.indexCount    = idxCount;
        dgcToken.instanceCount = 1;
        dgcToken.firstIndex    = baseIndex;
        dgcToken.vertexOffset  = (i32)baseVertex;
        dgcToken.firstInstance = (u32)m_DGCTokens.size();  // 初始时 firstInstance=objectIndex
        dgcToken.objectIndex   = (u32)m_DGCTokens.size();  // 按 Build 顺序递增
        m_DGCTokens.push_back(dgcToken);

        baseVertex += vtxCount;
        baseIndex  += idxCount;
    };

    // 收集集（与旧实现的遍历范围**逐条一致**，用 `SnapshotMeshClass` 表达）：
    // `Base`/`Cube`/`Sphere`/`Billboard`/`Text`/(`Decal`)/`Instanced`；**不含** `Spline`/`Skeletal`。
    // 顺序由快照的枚举顺序保证（Mesh → Cube → Sphere → Billboard → Text → Decal → Spline →
    // Instanced → Skeletal，过滤后相对顺序与旧的 `ForEach<T>` 序列一致）。
    for (const SnapshotDrawItem& item : snapshot.draws) {
        switch (item.meshClass) {
        case SnapshotMeshClass::Base:
        case SnapshotMeshClass::Cube:
        case SnapshotMeshClass::Sphere:
        case SnapshotMeshClass::Billboard:
        case SnapshotMeshClass::Text:
        case SnapshotMeshClass::Instanced:
            collect(item);
            break;
        case SnapshotMeshClass::Decal:
            if (!excludeDecals) collect(item);   // 任务 24：Deferred 排除贴花卡片
            break;
        default:
            break;   // Spline / Skeletal：不并入合批（骨骼顶点布局不同；样条不在旧收集集内）
        }
    }

    m_TotalVertices = (u32)m_MergedVertices.size();
    m_TotalIndices  = (u32)m_MergedIndices.size();

    // 创建合并后的 GPU 缓冲
    rhi::BufferDesc vbDesc;
    vbDesc.size = m_MergedVertices.size() * sizeof(StaticVertex);
    vbDesc.usage = rhi::BufferUsage::Vertex;
    vbDesc.initialData = m_MergedVertices.data();
    vbDesc.stride = sizeof(StaticVertex);
    m_MergedVB = rhi::GetDevice()->CreateBuffer(vbDesc);

    rhi::BufferDesc ibDesc;
    ibDesc.size = m_MergedIndices.size() * sizeof(u32);
    ibDesc.usage = rhi::BufferUsage::Index;
    ibDesc.initialData = m_MergedIndices.data();
    ibDesc.stride = sizeof(u32);
    m_MergedIB = rhi::GetDevice()->CreateBuffer(ibDesc);

    HE_CORE_INFO("MeshBatcher: {} meshes → {} verts, {} indices ({} KB VB + {} KB IB)",
        m_Commands.size(), m_TotalVertices, m_TotalIndices,
        (m_TotalVertices * sizeof(StaticVertex)) / 1024,
        (m_TotalIndices * sizeof(u32)) / 1024);
    return true;
}

void MeshBatcher::FillGPUScene(GPUScene& scene) const {
    auto& objs = const_cast<std::vector<GPUSceneObject>&>(scene.GetObjects());
    u32 count = std::min((u32)m_Commands.size(), (u32)objs.size());
    for (u32 i = 0; i < count; ++i) {
        objs[i].indexCount  = m_Commands[i].indexCount;
        objs[i].firstIndex  = m_Commands[i].firstIndex;
        objs[i].vertexOffset = m_Commands[i].vertexOffset;
    }
}

} // namespace he::render
