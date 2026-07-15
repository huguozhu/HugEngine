#pragma once

#include "RHI/RHI.h"
#include <vector>
#include <memory>
#include <functional>

// ============================================================
// GPUWorkGraph — GPU Work Graph 软件模拟框架
//
// 使用 Compute Shader + 原子计数器模拟 GPU 端工作调度。
// 支持三种节点类型：
//   Entry:  入口节点，读取输入 Record → 输出 Record（如遮挡剔除）
//   Compute:通用 Compute 节点，处理上游 Record → 输出下游 Record
//   Draw:   绘制节点，读取 Record → 执行间接绘制（暂收集统计）
//
// WGRecord = 32 字节（nodeID + 7×u32 payload）
// 每帧调用 PushEntryWork → Execute 完成一次 Work Graph 遍历。
// ============================================================

namespace he::render {

// ── Work Graph 节点类型 ──
enum class WGNodeType : u8 {
    Entry,     // 入口节点（Compute Shader）
    Compute,   // 通用 Compute 节点
    Draw,      // 绘制节点（Mesh/Draw）
};

// ── 节点间传递的 Record（GPU 端格式）──
// 32 字节对齐，匹配 Shader 端布局
struct alignas(16) WGRecord {
    u32 nodeID;       // 目标节点 ID
    u32 payload[7];   // 负载数据（最大 28 bytes）
};

// ── 节点描述符 ──
struct WGNodeDesc {
    const char* name;           // 调试名称
    WGNodeType  type;           // 节点类型
    const char* shaderPath;     // Shader 路径（Entry/Compute 节点）
    u32         maxRecords;     // 最大输入 Record 数
    u32         maxOutputs;     // 最大输出 Record 数
};

// ── GPU Work Graph 主类 ──
class GPUWorkGraph {
public:
    GPUWorkGraph() = default;
    ~GPUWorkGraph() = default;

    /// 初始化 Work Graph 系统
    bool Initialize(rhi::IRHIDevice* device);
    /// 关闭并释放所有 GPU 资源
    void Shutdown(rhi::IRHIDevice* device);

    /// 注册节点（返回节点 ID，ID = m_Nodes 索引）
    u32 AddNode(const WGNodeDesc& desc);

    /// 为节点设置自定义 Compute Shader（覆盖默认）
    /// @param bytecode 已编译的 SPIR-V ShaderBytecode（stage 必须是 Compute）
    void SetNodeShader(u32 nodeID, const rhi::ShaderBytecode& bytecode);

    /// 每帧：将初始工作推入入口节点的输入缓冲
    /// @param records WGRecord 数组
    /// @param count   Record 数量
    void PushEntryWork(u32 nodeID, const void* records, u32 count);

    /// 执行 Work Graph（单帧）
    /// 按节点注册顺序遍历，依次执行：
    ///   Entry/Compute: Dispatch Compute Shader + Barrier
    ///   Draw:          读取输入 Record，统计绘制数（暂作统计）
    void Execute(rhi::IRHICommandList* cmd);

    /// 获取节点数量
    u32 GetNodeCount() const { return (u32)m_Nodes.size(); }

    /// 获取可见记录数（最后执行的 Draw 节点的输入记录数）
    u32 GetLastVisibleCount() const { return m_LastVisibleCount; }

private:
    // 节点状态（GPU 资源 + 描述符）
    struct NodeState {
        WGNodeDesc desc;
        u32 nodeID;

        // Compute Shader PSO + 描述符（仅 Entry/Compute 节点）
        rhi::ShaderBytecode shaderBytecode;
        std::unique_ptr<rhi::IRHIPipelineState> pso;
        rhi::DescriptorSetLayoutHandle descLayout = rhi::kInvalidLayout;
        rhi::DescriptorSetHandle       descSet    = rhi::kInvalidSet;

        // GPU 端 Record 队列
        std::unique_ptr<rhi::IRHIBuffer> inputBuffer;   // 输入 Record[]
        std::unique_ptr<rhi::IRHIBuffer> outputBuffer;  // 输出 Record[]
        std::unique_ptr<rhi::IRHIBuffer> counterBuffer; // 原子计数器（u32[4]）
    };

    std::vector<NodeState> m_Nodes;
    rhi::IRHIDevice* m_Device = nullptr;
    bool m_Initialized = false;
    u32  m_LastVisibleCount = 0;  // 最后可见记录数（调试/统计用）
};

} // namespace he::render
