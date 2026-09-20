#pragma once

#include "Scene/MeshComponent.h"
#include "RHI/Buffer.h"
#include <vector>
#include <memory>

// ============================================================
// MeshBatcher — 合并所有 StaticVertex Mesh 到共享 VB+IB
//
// Build() 遍历 World，将全部 StaticVertex 网格合并到单个
// Vertex Buffer + Index Buffer，记录每个原始 mesh 的偏移。
// 输出 IndirectDrawCommand 数组，配合 ExecuteIndirect 使用。
//
// DGC 模式：扩展输出 DGCDrawToken（含 objectIndex），供 GPU Culling
// 写入 DGC preprocess buffer，配合 vkCmdExecuteGeneratedCommandsEXT 使用。
// ============================================================

namespace he::render {

// GPU 间接绘制命令（匹配 VkDrawIndexedIndirectCommand）
struct IndirectDrawCommand {
    u32 indexCount;
    u32 instanceCount;
    u32 firstIndex;
    i32 vertexOffset;
    u32 firstInstance;
};

// DGC 模式下 GPU Cull 输出格式（替代 IndirectDrawCommand，含 objectIndex）
struct alignas(16) DGCDrawToken {
    u32 indexCount;         // 索引数量
    u32 instanceCount;      // 实例数量（恒为 1）
    u32 firstIndex;         // 起始索引偏移
    i32 vertexOffset;       // 顶点偏移
    u32 firstInstance;      // 起始实例偏移（被 objectIndex 替代）
    u32 objectIndex;        // 物体索引（供 shader 查询材质等）
};

// ============================================================
// 【§14.8 任务 19】每个合并网格的**材质快照**（与 `GetDrawCommands()` 同序、同长）
//
// 【为什么需要它】Nanite 的资产是"整份合并几何当一个资产"，而材质是**逐网格**的（每个
//   MeshComponent 一份 PBR 参数 + 纹理路径）。任务 19 要把簇映射回源网格并取真实材质，
//   而 `IndirectDrawCommand` 里**没有**材质字段（核实：`MeshBatcher.cpp:58` 只写 5 个绘制参数，
//   材质信息原本只存在于 GPUScene 的 `materialIndex`）⇒ 在这里加一份**只读快照**，
//   与绘制区间一一对应（同一个 `collect` 调用里产出，顺序天然对齐）。
//
// 【字段口径】与 `SceneRenderer.cpp:110-130` 填 `GPUObjectData` 时**同一批来源**：
//   `MeshComponent::baseColorFactor / metallicFactor / roughnessFactor` + 5 条纹理路径
//   （后者按 `ComputeMaterialTextureMask` 压成位掩码）+ `materialID`（bindless 纹理基索引）。
//   于是软光栅按真实材质算出的 albedo/metallic/roughness 与既有 GBuffer 路径**同源**。
//
// 【只增不改】本结构不进入任何既有的绘制/上传结构体；`IndirectDrawCommand` 一个字节都没动
//   ⇒ 其它管线的行为与内存布局不变。
// ============================================================
struct MergedMeshMaterial {
    float baseColorFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };  ///< 基础色因子（RGBA）
    float metallicFactor     = 1.0f;   ///< 金属度因子
    float roughnessFactor    = 1.0f;   ///< 粗糙度因子
    u32   textureMask        = 0u;     ///< 纹理存在位掩码（与 ComputeMaterialTextureMask 同一套位）
    u32   bindlessTextureBase = 0u;    ///< bindless 纹理基索引（= MeshComponent::materialID）
};

class MeshBatcher {
public:
    /// @param excludeDecals 跳过贴花卡片（任务 24：Deferred 用 DecalPass 投影贴花）。
    ///        必须与 SceneRenderer::Prepare / GPUScene::Collect 的口径一致，否则 objectIndex 错位。
    bool Build(class World& world, bool excludeDecals = false);

    // 合并后的 GPU 缓冲
    rhi::IRHIBuffer* GetVertexBuffer() const { return m_MergedVB.get(); }
    rhi::IRHIBuffer* GetIndexBuffer()  const { return m_MergedIB.get(); }
    u32 GetTotalVertexCount() const { return m_TotalVertices; }
    u32 GetTotalIndexCount()  const { return m_TotalIndices; }

    // Indirect draw commands
    const std::vector<IndirectDrawCommand>& GetDrawCommands() const { return m_Commands; }
    u32 GetCommandCount() const { return (u32)m_Commands.size(); }

    /// 【§14.8 任务 19】逐合并网格的材质快照（与 `GetDrawCommands()` **同序、同长**）
    const std::vector<MergedMeshMaterial>& GetMeshMaterials() const { return m_MeshMaterials; }

    // ── Lumen Mesh SDF 构建（步骤 8）需要的 CPU 侧几何 ──
    // 合并缓冲本身是 Vertex/Index usage，不能当 Storage 读，故 SDF 构建要用这两份 CPU 数据
    // 另建自己的只读缓冲。几何**未施加任何变换**（与 GPUScene 的 per-object 变换配套），
    // 因此 SDF 建在**网格局部空间**，与 @ref GetDrawCommands 的 (firstIndex, vertexOffset) 配套。
    const std::vector<StaticVertex>& GetMergedVertices() const { return m_MergedVertices; }
    const std::vector<u32>&          GetMergedIndices()  const { return m_MergedIndices; }

    /// 将 draw 参数写入 GPUScene 对象（按 Build 顺序，objectIndex 匹配）
    void FillGPUScene(class GPUScene& scene) const;

    // ============================================================
    // DGC 模式支持
    // ============================================================

    /// DGC 模式开关（由管线根据硬件能力和 CVar 设置）
    bool useDGC = false;

    /// 获取 DGC draw tokens（GPU Cull 输出此数据，直接映射到 DGCDrawToken 数组）
    const std::vector<DGCDrawToken>& GetDGCTokens() const { return m_DGCTokens; }

private:
    std::vector<StaticVertex> m_MergedVertices;
    std::vector<u32>          m_MergedIndices;
    std::vector<IndirectDrawCommand> m_Commands;
    /// 【§14.8 任务 19】与 `m_Commands` 同序的材质快照（见 `MergedMeshMaterial`）
    std::vector<MergedMeshMaterial>  m_MeshMaterials;

    // DGC 模式的 draw token 列表（与 m_Commands 并行）
    std::vector<DGCDrawToken> m_DGCTokens;

    std::unique_ptr<rhi::IRHIBuffer> m_MergedVB;
    std::unique_ptr<rhi::IRHIBuffer> m_MergedIB;
    u32 m_TotalVertices = 0;
    u32 m_TotalIndices  = 0;
};

} // namespace he::render
