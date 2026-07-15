// Pipeline/MeshBatcher.cpp — Mesh 合并实现
#include "Pipeline/MeshBatcher.h"
#include "Pipeline/GPUScene.h"
#include "Scene/World.h"
#include "RHI/RHI.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Core/Log.h"

namespace he::render {

bool MeshBatcher::Build(World& world) {
    m_MergedVertices.clear();
    m_MergedIndices.clear();
    m_Commands.clear();
    m_DGCTokens.clear();

    u32 baseVertex = 0;
    u32 baseIndex  = 0;

    // 收集所有 StaticVertex mesh
    auto collect = [&](MeshComponent& mc) {
        u32 idxCount = mc.GetIndexCount();
        u32 vtxCount = mc.GetVertexCount();
        if (idxCount == 0 || vtxCount == 0) return;

        // 读取顶点数据
        auto* vb = mc.GetVertexBuffer().get();
        auto* ib = mc.GetIndexBuffer().get();
        if (!vb || !ib) return;

        // Map 原始 VB，拷贝顶点
        void* vData = vb->Map();
        if (vData) {
            u32 oldSize = (u32)m_MergedVertices.size();
            m_MergedVertices.resize(oldSize + vtxCount);
            memcpy(m_MergedVertices.data() + oldSize, vData, vtxCount * sizeof(StaticVertex));
            vb->Unmap();
        }

        // Map 原始 IB，偏移后拷贝索引
        void* iData = ib->Map();
        if (iData) {
            u32 oldSize = (u32)m_MergedIndices.size();
            m_MergedIndices.resize(oldSize + idxCount);
            auto* srcIndices = static_cast<u32*>(iData);
            for (u32 i = 0; i < idxCount; ++i)
                m_MergedIndices[oldSize + i] = srcIndices[i] + baseVertex;  // 偏移顶点索引
            ib->Unmap();
        }

        // 记录间接绘制命令
        m_Commands.push_back({idxCount, 1, baseIndex, (i32)baseVertex, 0});

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

    world.ForEach<MeshComponent>([&](Entity, MeshComponent& mc) { collect(mc); });
    world.ForEach<CubeComponent>([&](Entity, CubeComponent& cc) { collect(cc); });
    world.ForEach<SphereComponent>([&](Entity, SphereComponent& sc) { collect(sc); });

    m_TotalVertices = (u32)m_MergedVertices.size();
    m_TotalIndices  = (u32)m_MergedIndices.size();

    // 创建合并后的 GPU 缓冲
    rhi::BufferDesc vbDesc; vbDesc.size = m_MergedVertices.size() * sizeof(StaticVertex);
    vbDesc.usage = rhi::BufferUsage::Vertex; vbDesc.initialData = m_MergedVertices.data();
    vbDesc.stride = sizeof(StaticVertex);
    m_MergedVB = rhi::GetDevice()->CreateBuffer(vbDesc);

    rhi::BufferDesc ibDesc; ibDesc.size = m_MergedIndices.size() * sizeof(u32);
    ibDesc.usage = rhi::BufferUsage::Index; ibDesc.initialData = m_MergedIndices.data();
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
