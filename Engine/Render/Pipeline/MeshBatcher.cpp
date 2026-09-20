// Pipeline/MeshBatcher.cpp — Mesh 合并实现
#include "Pipeline/MeshBatcher.h"
#include "Pipeline/GPUScene.h"
#include "Pipeline/Material.h"   // 【任务 19】PBRMaterial / ComputeMaterialTextureMask（材质快照同源）
#include "Scene/World.h"
#include "RHI/RHI.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/BillboardComponent.h"
#include "Scene/TextRenderComponent.h"
#include "Scene/DecalComponent.h"
#include "Scene/InstancedMeshComponent.h"
#include "Scene/SkeletalMeshComponent.h"
#include "Core/Log.h"

namespace he::render {

bool MeshBatcher::Build(World& world, bool excludeDecals) {
    m_MergedVertices.clear();
    m_MergedIndices.clear();
    m_Commands.clear();
    m_DGCTokens.clear();
    m_MeshMaterials.clear();

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

        // 【§14.8 任务 19】材质快照：与上面那条命令**同一个 collect 调用**里产出 ⇒ 顺序天然对齐。
        // 字段来源与 `SceneRenderer.cpp:110-130` 填 GPUObjectData 时同一批：
        // 因子直接取组件的 PBR 字段，纹理掩码按"路径非空"压位（与 `ComputeMaterialTextureMask`
        // 同一套位序：bit0=BaseColor / bit1=Normal / bit2=MetallicRough / bit3=Occlusion）。
        {
            PBRMaterial mat = GetDefaultMaterial();
            mat.baseColorFactor         = mc.baseColorFactor;
            mat.metallicFactor          = mc.metallicFactor;
            mat.roughnessFactor         = mc.roughnessFactor;
            mat.baseColorTexture        = mc.baseColorTexture;
            mat.normalTexture           = mc.normalTexture;
            mat.metallicRoughnessTexture = mc.metallicRoughnessTexture;
            mat.occlusionTexture        = mc.occlusionTexture;

            MergedMeshMaterial material{};
            material.baseColorFactor[0] = mat.baseColorFactor.x;
            material.baseColorFactor[1] = mat.baseColorFactor.y;
            material.baseColorFactor[2] = mat.baseColorFactor.z;
            material.baseColorFactor[3] = mat.baseColorFactor.w;
            material.metallicFactor     = mat.metallicFactor;
            material.roughnessFactor    = mat.roughnessFactor;
            material.textureMask        = ComputeMaterialTextureMask(mat);
            material.bindlessTextureBase = mc.materialID;
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

    world.ForEach<MeshComponent>([&](Entity, MeshComponent& mc) { collect(mc); });
    world.ForEach<CubeComponent>([&](Entity, CubeComponent& cc) { collect(cc); });
    world.ForEach<SphereComponent>([&](Entity, SphereComponent& sc) { collect(sc); });
    // 广告牌/3D 文字/贴花：追加在最后（与 GPUScene::Collect 的枚举顺序一致，保证 objectIndex 对齐）
    world.ForEach<BillboardComponent>([&](Entity, BillboardComponent& bb) { collect(bb); });
    world.ForEach<TextRenderComponent>([&](Entity, TextRenderComponent& tr) { collect(tr); });
    // 任务 24：Deferred 排除贴花卡片（改由 DecalPass 投影），与 Prepare/GPUScene 口径一致
    if (!excludeDecals)
        world.ForEach<DecalComponent>([&](Entity, DecalComponent& dc) { collect(dc); });
    world.ForEach<InstancedMeshComponent>([&](Entity, InstancedMeshComponent& im) { collect(im); });
    // 骨骼网格：顶点布局不同（SkinnedVertex），不适合合批——跳过（蒙皮 Pass 单独绘制）
    world.ForEach<SkeletalMeshComponent>([&](Entity, SkeletalMeshComponent&) { /* 不并入合批 */ });

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
