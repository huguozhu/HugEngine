#pragma once

#include "RHI/Types.h"
#include "Core/Types.h"

#include <span>
#include <vector>

// ============================================================
// RHI Shader & Pipeline State Object
// ============================================================

namespace he::rhi {

// --- Push constant 范围描述 ---
// stageMask 使用 Vulkan VkShaderStageFlagBits 位掩码值：
//   1 = VERTEX, 16 = FRAGMENT, 32 = COMPUTE, ...
// 组合示例: stageMask = 1 | 16 → Vertex + Fragment 可见
struct PushConstantRange {
    u32 stageMask = 1;          // VK_SHADER_STAGE_VERTEX_BIT（默认 Vertex）
    u32 offset    = 0;          // 起始偏移（字节）
    u32 size      = 128;        // 大小（字节），最大 256
};

// --- Shader bytecode (pre-compiled) ---
struct ShaderBytecode {
    ShaderStage             stage = ShaderStage::Vertex;
    std::vector<u32>        spirv;    // SPIR-V binary
    // For D3D12: DXIL
    std::vector<u8>         dxil;
    String                  entryPoint = "main";
};

// --- Vertex input layout ---
enum class VertexFormat {
    Float, Float2, Float3, Float4,
    UByte4_Norm, Byte4_Norm,
    UInt, UInt2, UInt4,
};

struct VertexAttribute {
    u32             location = 0;
    u32             binding   = 0;
    VertexFormat    format    = VertexFormat::Float3;
    u32             offset    = 0;
};

struct VertexInputLayout {
    std::vector<VertexAttribute> attributes;
    u32 stride = 0; // Per-binding stride
};

// --- Pipeline State Object descriptor ---
struct PipelineStateDesc {
    // Shaders
    ShaderBytecode*     vertexShader   = nullptr;
    ShaderBytecode*     pixelShader    = nullptr;
    ShaderBytecode*     computeShader  = nullptr;
    // Mesh shading
    ShaderBytecode*     meshShader     = nullptr;
    ShaderBytecode*     amplificationShader = nullptr;

    // Vertex input
    VertexInputLayout   vertexLayout;

    // Render state
    PrimitiveTopology   topology        = PrimitiveTopology::TriangleList;
    bool                depthTest       = true;
    bool                depthWrite      = true;
    CompareFunc         depthCompare    = CompareFunc::LessEqual;

    // Render target
    u32                 colorAttachmentCount = 1;
    Format              colorFormats[kMaxColorAttachments] = {Format::RGBA8_UNORM};
    Format              depthFormat          = Format::D32_FLOAT;
    LoadOp              colorLoadOp          = LoadOp::Clear;  // 颜色附件加载操作：Clear=清屏, Load=保留内容
    LoadOp              depthLoadOp          = LoadOp::Clear;  // 深度附件加载操作

    // Multisampling
    u32                 sampleCount     = 1;

    // Pipeline bind point（图形 / 计算）
    PipelineBindPoint   bindPoint       = PipelineBindPoint::Graphics;

    // Push constant ranges（用于管线布局）
    std::vector<PushConstantRange> pushConstantRanges;

    // Descriptor set layouts（Pipeline Layout 使用，预先通过设备接口创建）
    std::vector<DescriptorSetLayoutHandle> descriptorSetLayouts;

    // Subpass index（默认 0，用于 Deferred 渲染的多 Subpass）
    u32                 subpassIndex    = 0;

    // Debug
    String              debugName;
};

// --- Pipeline State Object handle ---
class IRHIPipelineState {
public:
    virtual ~IRHIPipelineState() = default;

    /// 获取后端原生管线句柄（Vulkan: VkPipeline 转为 void*, D3D12: ID3D12PipelineState*）
    virtual void* GetNativeHandle() const { return nullptr; }
};

} // namespace he::rhi
