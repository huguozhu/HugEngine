#pragma once

#include "Core/Types.h"

// ============================================================
// RHI — fundamental enumerations and structures
// ============================================================

namespace he::rhi {

// --- Backend type ---
enum class Backend : u8 {
    Vulkan,
    D3D12,
    Metal,
    WebGPU,
    Count
};

// --- Queue type ---
enum class QueueType : u8 {
    Graphics   = 0,
    Compute    = 1,
    Copy       = 2,
    Count      = 3
};

// --- Texture format ---
enum class Format : u32 {
    Unknown = 0,

    // Color
    R8_UNORM,
    R8_SRGB,
    RG8_UNORM,
    RG8_SRGB,
    RGBA8_UNORM,
    RGBA8_SRGB,
    BGRA8_UNORM,
    BGRA8_SRGB,

    R16_FLOAT,
    RG16_FLOAT,
    RGBA16_FLOAT,

    R32_FLOAT,
    RG32_FLOAT,
    RGBA32_FLOAT,

    R11G11B10_FLOAT,

    // Depth / Stencil
    D16_UNORM,
    D32_FLOAT,
    D24_UNORM_S8_UINT,
    D32_FLOAT_S8_UINT,

    // Compressed (Block Compressed)
    BC1_UNORM,
    BC3_UNORM,
    BC4_UNORM,
    BC5_UNORM,
    BC7_UNORM,

    Count
};

// --- Buffer usage flags ---
enum class BufferUsage : u32 {
    None              = 0,
    Vertex            = 1 << 0,
    Index             = 1 << 1,
    Uniform           = 1 << 2,
    Storage           = 1 << 3,
    Indirect          = 1 << 4,
    AccelerationStruct= 1 << 5,
    TransferSrc       = 1 << 6,
    TransferDst       = 1 << 7,
};
inline BufferUsage operator|(BufferUsage a, BufferUsage b) { return BufferUsage(u32(a) | u32(b)); }
inline BufferUsage operator&(BufferUsage a, BufferUsage b) { return BufferUsage(u32(a) & u32(b)); }
inline bool      operator!(BufferUsage a)                  { return u32(a) == 0; }

// --- Texture usage flags ---
enum class TextureUsage : u32 {
    None              = 0,
    ShaderResource    = 1 << 0,
    RenderTarget      = 1 << 1,
    DepthStencil      = 1 << 2,
    UnorderedAccess   = 1 << 3,
    TransferSrc       = 1 << 4,
    TransferDst       = 1 << 5,
};
inline TextureUsage operator|(TextureUsage a, TextureUsage b) { return TextureUsage(u32(a) | u32(b)); }

// --- Shader stage ---
enum class ShaderStage : u8 {
    Vertex,
    Pixel,
    Compute,
    Geometry,
    Mesh,
    Amplification,
    RayGen,
    AnyHit,
    ClosestHit,
    Miss,
    Count
};

// --- Primitive topology ---
enum class PrimitiveTopology : u8 {
    TriangleList,
    TriangleStrip,
    LineList,
    PointList,
};

// --- Compare function ---
enum class CompareFunc : u8 {
    Never,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always,
};

// --- Device capabilities ---
struct DeviceCaps {
    u32     maxBindlessResources   = 0;
    u32     maxPushConstantsSize   = 128;
    u32     maxSamplerAnisotropy   = 16;
    bool    supportsRayTracing     = false;
    bool    supportsMeshShaders    = false;
    bool    supportsWorkGraphs     = false;
    bool    supportsVRS            = false;
    bool    supportsSER            = false;  // Shader Execution Reordering
    bool    supportsOMM            = false;  // Opacity Micromaps
    bool    supportsCooperativeVectors = false;
    bool    supportsLinearAlgebra  = false;
    bool    supportsSamplerFeedback = false;
};

} // namespace he::rhi
