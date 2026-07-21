#include "Pipeline/GPUWorkGraph.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include <cstring>

// ── 内嵌 WorkGraph_Entry 着色器（默认 Entry 节点 Compute Shader）──
// 由 shader 编译管线生成：slangc → SPIR-V → spv_to_header.py → .spv.h
#include "WorkGraph_Entry.comp.spv.h"

namespace he::render {

// ============================================================
// Initialize — 初始化 Work Graph 系统
// ============================================================

bool GPUWorkGraph::Initialize(rhi::IRHIDevice* device) {
    if (!device) {
        HE_CORE_ERROR("GPUWorkGraph: 设备指针为空");
        return false;
    }

    m_Device = device;
    m_Initialized = true;
    m_LastVisibleCount = 0;

    HE_CORE_INFO("GPUWorkGraph: 初始化完成");
    return true;
}

// ============================================================
// Shutdown — 清理所有 GPU 资源
// ============================================================

void GPUWorkGraph::Shutdown(rhi::IRHIDevice* device) {
    if (!m_Initialized) return;

    // 清理每个节点的 GPU 资源
    for (auto& node : m_Nodes) {
        // 销毁描述符集布局
        if (node.descLayout != rhi::kInvalidLayout) {
            device->DestroyDescriptorSetLayout(node.descLayout);
            node.descLayout = rhi::kInvalidLayout;
        }
        node.descSet = rhi::kInvalidSet;

        // 释放 GPU 缓冲
        node.inputBuffer.reset();
        node.outputBuffer.reset();
        node.counterBuffer.reset();

        // 释放 PSO
        node.pso.reset();
    }

    m_Nodes.clear();
    m_Device = nullptr;
    m_Initialized = false;

    HE_CORE_INFO("GPUWorkGraph: 已关闭");
}

// ============================================================
// AddNode — 注册新节点
// ============================================================

u32 GPUWorkGraph::AddNode(const WGNodeDesc& desc) {
    HE_ASSERT(m_Initialized, "GPUWorkGraph: 未初始化");
    HE_ASSERT(desc.maxRecords > 0, "GPUWorkGraph: maxRecords 必须 > 0");

    NodeState node;
    node.nodeID = (u32)m_Nodes.size();
    node.desc = desc;

    // ── Entry 节点：加载默认 Compute Shader + 创建 GPU 资源 ──
    if (desc.type == WGNodeType::Entry) {
        // 加载默认 WorkGraph_Entry SPIR-V 着色器
        node.shaderBytecode.stage = rhi::ShaderStage::Compute;
        node.shaderBytecode.spirv = k_WorkGraph_Entry_comp_spv;
        node.shaderBytecode.entryPoint = "main";
    }

    // ── Entry / Compute 节点：创建描述符布局 + GPU 缓冲 ──
    if (desc.type == WGNodeType::Entry || desc.type == WGNodeType::Compute) {
        // 输入 Record 缓冲（CPU 写入 / 上游输出）
        {
            rhi::BufferDesc bd;
            bd.size     = sizeof(WGRecord) * desc.maxRecords;
            bd.usage    = rhi::BufferUsage::Storage;
            bd.cpuAccess = true;  // 允许 CPU Map/Unmap
            node.inputBuffer = m_Device->CreateBuffer(bd);
            HE_ASSERT(node.inputBuffer, "GPUWorkGraph: inputBuffer 创建失败");
        }

        // 输出 Record 缓冲（输出到下游节点）
        {
            rhi::BufferDesc bd;
            bd.size     = sizeof(WGRecord) * desc.maxOutputs;
            bd.usage    = rhi::BufferUsage::Storage;
            bd.cpuAccess = true;
            node.outputBuffer = m_Device->CreateBuffer(bd);
            HE_ASSERT(node.outputBuffer, "GPUWorkGraph: outputBuffer 创建失败");
        }

        // 原子计数器缓冲（u32[4]：counter[0]=输入计数, counter[1]=输出计数）
        {
            rhi::BufferDesc bd;
            bd.size     = sizeof(u32) * 4;
            bd.usage    = rhi::BufferUsage::Storage;
            bd.cpuAccess = true;
            node.counterBuffer = m_Device->CreateBuffer(bd);
            HE_ASSERT(node.counterBuffer, "GPUWorkGraph: counterBuffer 创建失败");

            // 初始清零
            void* ptr = node.counterBuffer->Map();
            if (ptr) {
                std::memset(ptr, 0, sizeof(u32) * 4);
                node.counterBuffer->Unmap();
            }
        }

        // ── 创建描述符布局（3 个 StorageBuffer binding）──
        rhi::DescriptorSetLayoutDesc layoutDesc;
        layoutDesc.bindings = {
            {0, rhi::DescriptorType::StorageBuffer, 1, 32},  // input WGRecord[]
            {1, rhi::DescriptorType::StorageBuffer, 1, 32},  // output WGRecord[]
            {2, rhi::DescriptorType::StorageBuffer, 1, 32},  // counter u32[4]
        };
        node.descLayout = m_Device->CreateDescriptorSetLayout(layoutDesc);
        HE_ASSERT(node.descLayout != rhi::kInvalidLayout,
                  "GPUWorkGraph: descLayout 创建失败");

        // 分配描述符集并绑定缓冲
        node.descSet = m_Device->AllocateDescriptorSet(node.descLayout);
        HE_ASSERT(node.descSet != rhi::kInvalidSet,
                  "GPUWorkGraph: descSet 分配失败");

        m_Device->UpdateDescriptorSet(node.descSet, 0,
            rhi::DescriptorType::StorageBuffer, node.inputBuffer.get());
        m_Device->UpdateDescriptorSet(node.descSet, 1,
            rhi::DescriptorType::StorageBuffer, node.outputBuffer.get());
        m_Device->UpdateDescriptorSet(node.descSet, 2,
            rhi::DescriptorType::StorageBuffer, node.counterBuffer.get());

        // ── 创建 Compute PSO（Entry 节点使用默认 WorkGraph_Entry shader）──
        if (desc.type == WGNodeType::Entry) {
            rhi::PushConstantRange pcRange;
            pcRange.stageMask = 32;  // VK_SHADER_STAGE_COMPUTE_BIT
            pcRange.offset = 0;
            pcRange.size   = 16;    // 4×u32: targetNodeID + maxOutputs + inputCount + _pad

            rhi::PipelineStateDesc psoDesc;
            psoDesc.bindPoint = rhi::PipelineBindPoint::Compute;
            psoDesc.computeShader = &node.shaderBytecode;
            psoDesc.pushConstantRanges = {pcRange};
            psoDesc.descriptorSetLayouts = {node.descLayout};
            psoDesc.debugName = desc.name ? desc.name : "WorkGraph_Entry";

            node.pso = m_Device->CreatePipelineState(psoDesc);
            if (!node.pso) {
                HE_CORE_WARN("GPUWorkGraph: 节点 \"{}\" 的 PSO 创建失败",
                             desc.name ? desc.name : "(unnamed)");
            }
        }
        // Compute 节点：PSO 由 SetNodeShader 设置
    }

    // ── Draw 节点：仅创建输入 Record 缓冲 ──
    else if (desc.type == WGNodeType::Draw) {
        {
            rhi::BufferDesc bd;
            bd.size     = sizeof(WGRecord) * desc.maxRecords;
            bd.usage    = rhi::BufferUsage::Storage;
            bd.cpuAccess = true;
            node.inputBuffer = m_Device->CreateBuffer(bd);
            HE_ASSERT(node.inputBuffer, "GPUWorkGraph: Draw 节点 inputBuffer 创建失败");
        }
        // Draw 节点也创建输出缓冲（统一接口，供统计使用）
        {
            rhi::BufferDesc bd;
            bd.size     = sizeof(WGRecord) * desc.maxOutputs;
            bd.usage    = rhi::BufferUsage::Storage;
            bd.cpuAccess = true;
            node.outputBuffer = m_Device->CreateBuffer(bd);
        }
    }

    m_Nodes.push_back(std::move(node));

    HE_CORE_INFO("GPUWorkGraph: 添加节点 [{}] \"{}\" type={} maxRec={} maxOut={}",
                 node.nodeID,
                 desc.name ? desc.name : "(unnamed)",
                 static_cast<u8>(desc.type),
                 desc.maxRecords,
                 desc.maxOutputs);

    return node.nodeID;
}

// ============================================================
// SetNodeShader — 为节点设置自定义 Compute Shader
// ============================================================

void GPUWorkGraph::SetNodeShader(u32 nodeID, const rhi::ShaderBytecode& bytecode) {
    if (!m_Initialized || nodeID >= (u32)m_Nodes.size()) {
        HE_CORE_WARN("GPUWorkGraph: SetNodeShader 无效节点 ID {}", nodeID);
        return;
    }

    auto& node = m_Nodes[nodeID];

    // 仅 Entry/Compute 节点支持设置 Shader
    if (node.desc.type != WGNodeType::Entry && node.desc.type != WGNodeType::Compute) {
        HE_CORE_WARN("GPUWorkGraph: 节点 \"{}\" 不是 Entry/Compute 类型，忽略 SetNodeShader",
                     node.desc.name ? node.desc.name : "(unnamed)");
        return;
    }

    // 复制着色器字节码
    node.shaderBytecode = bytecode;
    node.shaderBytecode.stage = rhi::ShaderStage::Compute;

    // 重建 PSO
    node.pso.reset();  // 释放旧 PSO

    rhi::PushConstantRange pcRange;
    pcRange.stageMask = 32;  // VK_SHADER_STAGE_COMPUTE_BIT
    pcRange.offset = 0;
    pcRange.size   = 16;     // 4×u32

    rhi::PipelineStateDesc psoDesc;
    psoDesc.bindPoint = rhi::PipelineBindPoint::Compute;
    psoDesc.computeShader = &node.shaderBytecode;
    psoDesc.pushConstantRanges = {pcRange};
    psoDesc.descriptorSetLayouts = {node.descLayout};
    psoDesc.debugName = node.desc.name ? node.desc.name : "WorkGraph_Node";

    node.pso = m_Device->CreatePipelineState(psoDesc);
    if (!node.pso) {
        HE_CORE_WARN("GPUWorkGraph: SetNodeShader PSO 创建失败 (节点 {})",
                     node.desc.name ? node.desc.name : "(unnamed)");
    } else {
        HE_CORE_INFO("GPUWorkGraph: 节点 \"{}\" Shader 已更新",
                     node.desc.name ? node.desc.name : "(unnamed)");
    }
}

// ============================================================
// PushEntryWork — 将 CPU 端 Record 推入节点输入缓冲
// ============================================================

void GPUWorkGraph::PushEntryWork(u32 nodeID, const void* records, u32 count) {
    if (!m_Initialized || nodeID >= (u32)m_Nodes.size()) return;

    auto& node = m_Nodes[nodeID];
    HE_ASSERT(node.desc.type == WGNodeType::Entry,
              "GPUWorkGraph: PushEntryWork 目标节点不是 Entry 类型");

    if (count == 0 || !records) return;
    if (count > node.desc.maxRecords) {
        HE_CORE_WARN("GPUWorkGraph: PushEntryWork 裁剪输入 {} → {} (maxRecords)",
                     count, node.desc.maxRecords);
        count = node.desc.maxRecords;
    }

    // 拷贝 Record 数据到输入缓冲
    void* mapped = node.inputBuffer->Map();
    if (mapped) {
        std::memcpy(mapped, records, count * sizeof(WGRecord));
        node.inputBuffer->Unmap();
    }

    // 写入计数器：counter[0] = 输入 Record 数，counter[1] = 输出 Record 数（清零）
    mapped = node.counterBuffer->Map();
    if (mapped) {
        auto* counters = static_cast<u32*>(mapped);
        counters[0] = count;  // 输入 Record 数（供 shader 读取）
        counters[1] = 0;      // 输出 Record 数（清零）
        node.counterBuffer->Unmap();
    }
}

// ============================================================
// Execute — 执行 Work Graph（遍历所有节点）
//
// 执行顺序与 AddNode 注册顺序一致（拓扑顺序由用户保证）。
// 每帧调用一次，依次执行每个节点。
// ============================================================

void GPUWorkGraph::Execute(rhi::IRHICommandList* cmd) {
    if (!m_Initialized || m_Nodes.empty()) return;

    m_LastVisibleCount = 0;

    // 遍历节点，按拓扑顺序执行
    for (auto& node : m_Nodes) {
        switch (node.desc.type) {

        // ── Entry / Compute 节点：Dispatch Compute Shader ──
        case WGNodeType::Entry:
        case WGNodeType::Compute: {
            if (!node.pso) {
                HE_CORE_WARN("GPUWorkGraph: 节点 \"{}\" 无 PSO，跳过 Dispatch",
                             node.desc.name ? node.desc.name : "(unnamed)");
                continue;
            }

            // 清零输出计数器（counter[1] = 0）
            void* mapped = node.counterBuffer->Map();
            if (mapped) {
                auto* counters = static_cast<u32*>(mapped);
                counters[1] = 0;  // 输出计数清零
                node.counterBuffer->Unmap();
            }

            // 绑定 PSO + 描述符集
            cmd->SetPipeline(node.pso.get());
            cmd->BindDescriptorSet(rhi::kDescSetPerFrame, node.descSet);

            // Push Constants：4×u32
            //   targetNodeID — 当前节点 ID（shader 中用于输出记录的目标节点 ID）
            //   maxOutputs   — 最大输出记录数
            //   inputCount   — 输入记录数
            //   _pad         — 填充
            struct WGPushConst {
                u32 targetNodeID;
                u32 maxOutputs;
                u32 inputCount;
                u32 _pad;
            };
            WGPushConst pc;
            pc.targetNodeID = node.nodeID;
            pc.maxOutputs   = node.desc.maxOutputs;
            pc.inputCount   = node.desc.maxRecords;
            pc._pad         = 0;
            cmd->SetPushConstants(0, sizeof(pc), &pc);

            // Dispatch（每 64 条 Record 一个线程组）
            u32 groups = (node.desc.maxRecords + 63) / 64;
            if (groups > 0) {
                cmd->Dispatch(groups, 1, 1);
            }

            // Barrier：确保 Compute Shader 写入对后续节点可见
            cmd->PipelineBarrier(
                rhi::PipelineStage::ComputeShader,
                rhi::PipelineStage::ComputeShader,
                rhi::ResourceState::UnorderedAccess,
                rhi::ResourceState::UnorderedAccess);

            break;
        }

        // ── Draw 节点：读取输入 Record，统计绘制 ──
        case WGNodeType::Draw: {
            // 如果是第一个节点或前驱节点的输出已准备就绪，
            // 将前驱节点的输出拷贝到 Draw 节点的输入
            //（当前简化：Draw 节点直接读取自己的 inputBuffer）

            void* mapped = node.inputBuffer->Map();
            if (!mapped) {
                HE_CORE_WARN("GPUWorkGraph: Draw 节点 \"{}\" inputBuffer Map 失败",
                             node.desc.name ? node.desc.name : "(unnamed)");
                continue;
            }

            auto* records = static_cast<WGRecord*>(mapped);
            u32 recordCount = node.desc.maxRecords;

            // 统计非空 Record（nodeID != 0xFFFFFFFF 表示有效）
            u32 validCount = 0;
            for (u32 i = 0; i < recordCount; ++i) {
                if (records[i].nodeID != 0xFFFFFFFF) {
                    validCount++;
                }
            }
            node.inputBuffer->Unmap();

            m_LastVisibleCount = validCount;

            HE_CORE_TRACE("GPUWorkGraph: Draw 节点 \"{}\" — {} 个有效 Record",
                          node.desc.name ? node.desc.name : "(unnamed)",
                          validCount);
            break;
        }
        }
    }
}

} // namespace he::render
