#pragma once

#include "RHI/Types.h"
#include "Core/Types.h"

#include <span>
#include <vector>

// ============================================================
// RHI Shader & Pipeline State Object
// ============================================================

namespace he::rhi {

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
    Format              colorFormats[8]      = {Format::RGBA8_UNORM};
    Format              depthFormat          = Format::D32_FLOAT;

    // Multisampling
    u32                 sampleCount     = 1;

    // Debug
    String              debugName;
};

// --- Pipeline State Object handle ---
class IRHIPipelineState {
public:
    virtual ~IRHIPipelineState() = default;
};

} // namespace he::rhi
