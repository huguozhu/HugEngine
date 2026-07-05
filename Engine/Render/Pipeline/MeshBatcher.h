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

private:
    std::vector<StaticVertex> m_MergedVertices;
    std::vector<u32>          m_MergedIndices;
    std::vector<IndirectDrawCommand> m_Commands;

    std::unique_ptr<rhi::IRHIBuffer> m_MergedVB;
    std::unique_ptr<rhi::IRHIBuffer> m_MergedIB;
    u32 m_TotalVertices = 0;
    u32 m_TotalIndices  = 0;
};

} // namespace he::render
