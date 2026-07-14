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

class MeshBatcher {
public:
    bool Build(class World& world);

    // 合并后的 GPU 缓冲
    rhi::IRHIBuffer* GetVertexBuffer() const { return m_MergedVB.get(); }
    rhi::IRHIBuffer* GetIndexBuffer()  const { return m_MergedIB.get(); }
    u32 GetTotalVertexCount() const { return m_TotalVertices; }
    u32 GetTotalIndexCount()  const { return m_TotalIndices; }

    // Indirect draw commands
    const std::vector<IndirectDrawCommand>& GetDrawCommands() const { return m_Commands; }
    u32 GetCommandCount() const { return (u32)m_Commands.size(); }

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

    // DGC 模式的 draw token 列表（与 m_Commands 并行）
    std::vector<DGCDrawToken> m_DGCTokens;

    std::unique_ptr<rhi::IRHIBuffer> m_MergedVB;
    std::unique_ptr<rhi::IRHIBuffer> m_MergedIB;
    u32 m_TotalVertices = 0;
    u32 m_TotalIndices  = 0;
};

} // namespace he::render
