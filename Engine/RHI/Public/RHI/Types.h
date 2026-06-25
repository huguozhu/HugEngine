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

// --- Sampler filter mode ---
enum class FilterMode : u8 {
    Nearest,  // 最近点采样
    Linear,   // 双线性插值
};

// --- Sampler address mode ---
enum class AddressMode : u8 {
    Repeat,          // 重复
    MirroredRepeat,  // 镜像重复
    ClampToEdge,     // 边缘钳制
    ClampToBorder,   // 边界色钳制
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

// --- 管线绑定点 ---
enum class PipelineBindPoint : u8 {
    Graphics,   // 图形管线
    Compute,    // 计算管线
};

// --- 描述符类型 ---
enum class DescriptorType : u8 {
    UniformBuffer,              // 常量缓冲区
    StorageBuffer,              // 结构化缓冲区（读写）
    CombinedImageSampler,       // 组合图像+采样器
    StorageImage,               // 存储图像（读写）
    Sampler,                    // 独立采样器
    InputAttachment,            // 输入附件（用于 SubPass）
};

// --- 描述符集布局 ---
struct DescriptorSetLayoutBinding {
    u32             binding     = 0;        // binding 编号
    DescriptorType  type        = DescriptorType::UniformBuffer;
    u32             count       = 1;        // 数组元素数（bindless 时可能很大）
    ShaderStage     stageFlags  = ShaderStage::Vertex;  // 可见的着色器阶段
    bool            bindless    = false;    // 是否为无绑定数组（变长 descriptor count）
};

struct DescriptorSetLayoutDesc {
    std::vector<DescriptorSetLayoutBinding> bindings;
};

// --- 资源状态（用于 Barrier 推导）---
enum class ResourceState : u32 {
    Undefined               = 0,
    // 通用
    Common                  = 1 << 0,
    // 顶点/索引缓冲
    VertexAndConstantBuffer = 1 << 1,
    IndexBuffer             = 1 << 2,
    // 渲染目标
    RenderTarget            = 1 << 3,
    // 深度模板
    DepthStencilRead        = 1 << 4,
    DepthStencilWrite       = 1 << 5,
    // 着色器资源
    ShaderResource          = 1 << 6,
    UnorderedAccess         = 1 << 7,
    // 拷贝
    CopySrc                 = 1 << 8,
    CopyDst                 = 1 << 9,
    // 呈现
    Present                 = 1 << 10,
    // 间接绘制
    IndirectArgument        = 1 << 11,
};

inline ResourceState operator|(ResourceState a, ResourceState b) { return ResourceState(u32(a) | u32(b)); }
inline ResourceState operator&(ResourceState a, ResourceState b) { return ResourceState(u32(a) & u32(b)); }
inline bool          operator!(ResourceState a)                  { return u32(a) == 0; }

// --- 管线阶段标志（用于 Barrier）---
enum class PipelineStage : u32 {
    None                        = 0,
    TopOfPipe                   = 1 << 0,
    DrawIndirect                = 1 << 1,
    VertexInput                 = 1 << 2,
    VertexShader                = 1 << 3,
    FragmentShader              = 1 << 4,
    EarlyFragmentTests          = 1 << 5,
    LateFragmentTests           = 1 << 6,
    ColorAttachmentOutput       = 1 << 7,
    ComputeShader               = 1 << 8,
    Transfer                    = 1 << 9,
    BottomOfPipe                = 1 << 10,
};

inline PipelineStage operator|(PipelineStage a, PipelineStage b) { return PipelineStage(u32(a) | u32(b)); }

// --- 访问标志（用于 Barrier）---
enum class AccessFlags : u32 {
    None                        = 0,
    IndirectCommandRead         = 1 << 0,
    IndexRead                   = 1 << 1,
    VertexAttributeRead         = 1 << 2,
    UniformRead                 = 1 << 3,
    ShaderRead                  = 1 << 4,
    ShaderWrite                 = 1 << 5,
    ColorAttachmentRead         = 1 << 6,
    ColorAttachmentWrite        = 1 << 7,
    DepthStencilAttachmentRead  = 1 << 8,
    DepthStencilAttachmentWrite = 1 << 9,
    TransferRead                = 1 << 10,
    TransferWrite               = 1 << 11,
    MemoryRead                  = 1 << 12,
    MemoryWrite                 = 1 << 13,
};

inline AccessFlags operator|(AccessFlags a, AccessFlags b) { return AccessFlags(u32(a) | u32(b)); }

// --- 间接绘制命令（GPU 可写结构）---
struct IndirectDrawCommand {
    u32 vertexCount;        // 顶点数量
    u32 instanceCount;      // 实例数量
    u32 firstVertex;        // 起始顶点偏移
    u32 firstInstance;      // 起始实例偏移
};

struct IndirectDrawIndexedCommand {
    u32 indexCount;         // 索引数量
    u32 instanceCount;      // 实例数量
    u32 firstIndex;         // 起始索引偏移
    i32 vertexOffset;       // 顶点偏移
    u32 firstInstance;      // 起始实例偏移
};

// --- 计算间接调度命令 ---
struct DispatchIndirectCommand {
    u32 groupCountX;        // X 方向工作组数
    u32 groupCountY;        // Y 方向工作组数
    u32 groupCountZ;        // Z 方向工作组数
};

} // namespace he::rhi
