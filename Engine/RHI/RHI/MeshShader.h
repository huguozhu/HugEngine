#pragma once

#include "RHI/Types.h"
#include "RHI/Shader.h"
#include "Core/Types.h"

#include <vector>

// ============================================================
// RHI Mesh Shader — Mesh Pipeline State 描述
// ============================================================

namespace he::rhi {

// --- Mesh Shader 管线状态描述 ---
// 与普通图形管线不同：无 IA（Input Assembly）阶段，无 Vertex Input Layout
// Mesh Shader 自行通过 Storage Buffer 读取顶点数据并输出几何体
struct MeshPipelineStateDesc {
    ShaderBytecode* meshShader            = nullptr;  // Mesh Shader（必须）
    ShaderBytecode* amplificationShader   = nullptr;  // Amplification Shader（可选，用于预剔除/LOD）
    ShaderBytecode* pixelShader           = nullptr;  // Pixel Shader（可选，片段着色）

    // 图元输出类型
    PrimitiveTopology topology    = PrimitiveTopology::TriangleList;
    bool              outputPoints = false;  // 输出点图元
    bool              outputLines  = false;  // 输出线图元

    // 深度状态
    bool        depthTest    = true;
    bool        depthWrite   = true;
    CompareFunc depthCompare = CompareFunc::LessEqual;

    // 渲染目标
    u32    colorAttachmentCount        = 1;
    Format colorFormats[kMaxColorAttachments]             = {Format::RGBA8_UNORM};
    Format depthFormat                 = Format::D32_FLOAT;

    // 多重采样
    u32    sampleCount = 1;

    // Push Constant 范围
    std::vector<PushConstantRange> pushConstantRanges;

    // 描述符集布局
    std::vector<DescriptorSetLayoutHandle> descriptorSetLayouts;

    // 调试名称
    String debugName;
};

} // namespace he::rhi
