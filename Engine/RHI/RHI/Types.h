#pragma once

#include "Core/Types.h"

// ============================================================
// RHI — fundamental enumerations and structures
//
// 与 Slang Shader 共享的常量（kDescSet*/kBinding*/kMax* 等）
// 定义在 ShaderTypes.slang 的 he::gpu 命名空间中。
// C++ 端保持独立的常量定义（值必须与 shader 端严格一致）。
// 修改这些值时，必须同步更新 ShaderTypes.slang。
// ============================================================

namespace he::rhi {

// 最大飞行帧数（Triple Buffering）
constexpr u32 kMaxFramesInFlight = 3;

// 默认 BackBuffer 分辨率（1080p），引用 Core 层统一定义
constexpr u32 kDefaultBackBufferWidth  = kDefaultWindowWidth;
constexpr u32 kDefaultBackBufferHeight = kDefaultWindowHeight;

// 描述符集索引常量（与 ShaderTypes: kGPUDescSet_* 一致）
constexpr u32 kDescSetPerFrame = 0;  // set=0: 逐帧数据（Camera、Lights、Shadows 等）
constexpr u32 kDescSetMaterial = 1;  // set=1: 逐材质数据（Bindless Textures 等）
constexpr u32 kDescSetBindless = 2;  // set=2: 无绑定资源（TLAS 等）

// 共享 Descriptor Binding 常量（与 ShaderTypes: kGPUBinding_* 一致）
constexpr u32 kBindingObjectData       = 2;   // Object SSBO（GBuffer / Shadow Pass 共用）
constexpr u32 kBindingBindlessTextures = 5;   // Bindless 纹理数组（SampledImage）
constexpr u32 kBindingBindlessSamplers = 6;   // Bindless 采样器数组（Sampler）
constexpr u32 kBindingLightGrid        = 7;   // 光源网格（Clustered Shading SSBO）
constexpr u32 kBindingLightIndexList   = 8;   // 光源索引列表（Clustered Shading SSBO）

// 渲染管线限制常量（与 ShaderTypes: kGPU* 一致）
constexpr u32 kMaxColorAttachments  = 8;   // 最大 MRT 颜色附件数
constexpr u32 kMaxMeshShaderStages  = 3;   // 最大 Mesh Shader 管线阶段数
constexpr u32 kMaxShaderStages      = 2;   // 最大传统管线着色器阶段数

// Ray Tracing 管线参数常量（与 ShaderTypes: kGPURT* 一致）
constexpr u32 kRTMaxRecursionDepth   = 2;   // 最大递归深度
constexpr u32 kRTMaxPayloadSize      = 16;  // 最大 Payload 字节数
constexpr u32 kRTMaxHitAttributeSize = 8;   // 最大 Hit Attribute 字节数
constexpr u32 kMaxProfilerPasses     = 20;  // GPU Profiler 最大记录 Pass 数
constexpr u32 kMaxPushConstantSize   = 256; // Push Constant 最大字节数
constexpr u32 kDefaultPushConstantSize  = 128;    // Push Constant 默认范围大小
constexpr u32 kDefaultMaxBindlessResources = 1000000; // Bindless 资源默认容量上限（C++ 独有）
constexpr u32 kCubemapFaceCount        = 6;      // 立方体贴图面数
constexpr u32 kMaxConcurrentSemaphores = 2;   // 最多并发信号量数（C++ 独有）
constexpr u32 kRTShaderUnused         = ~0u; // RT 着色器组中"未绑定"槽位哨兵

// Shader Stage 位掩码常量（当前映射 Vulkan VkShaderStageFlagBits，未来适配 D3D12/Metal）
// 传统管线
constexpr u32 kStageMaskVertex        = 1;       // Vertex Shader
constexpr u32 kStageMaskGeometry      = 8;       // Geometry Shader
constexpr u32 kStageMaskFragment      = 16;      // Fragment / Pixel Shader
constexpr u32 kStageMaskCompute       = 32;      // Compute Shader
// Mesh Shader 管线
constexpr u32 kStageMaskMesh          = 64;      // Mesh Shader (VK_EXT_mesh_shader)
constexpr u32 kStageMaskAmplification = 128;     // Task / Amplification Shader
// Ray Tracing 管线
constexpr u32 kStageMaskRayGen        = 0x100;   // Ray Generation Shader
constexpr u32 kStageMaskAnyHit        = 0x200;   // Any-Hit Shader
constexpr u32 kStageMaskClosestHit    = 0x400;   // Closest-Hit Shader
constexpr u32 kStageMaskMiss          = 0x800;   // Miss Shader
constexpr u32 kStageMaskCallable      = 0x2000;  // Callable Shader

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

// --- Query pool 类型 ---
// Timestamp: GPU 时间戳（用于 Profiler 计时）
// PipelineStatistics: 硬件管线统计（VS/PS 调用次数、三角形数等）
enum class QueryType : u8 {
    Timestamp,           // VK_QUERY_TYPE_TIMESTAMP
    PipelineStatistics,  // VK_QUERY_TYPE_PIPELINE_STATISTICS
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
    RGB32_FLOAT,
    RGBA32_FLOAT,

    // 整数格式
    R32_UINT,

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
    Cubemap           = 1 << 6,  // 立方体贴图（6 面，用于点光源阴影等）
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
    Callable,
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

// --- 剔除模式 ---
enum class CullMode : u8 {
    None,         // 不剔除
    Front,        // 剔除正面
    Back,         // 剔除背面
    FrontAndBack, // 剔除正反面（仅渲染点/线）
};

// --- 正面朝向 ---
enum class FrontFace : u8 {
    Clockwise,        // 顺时针为正面（Vulkan: VK_FRONT_FACE_CLOCKWISE）
    CounterClockwise, // 逆时针为正面（D3D12/Metal 默认）
};

// --- 填充模式 ---
enum class FillMode : u8 {
    Solid,     // 实体填充
    Wireframe, // 线框
};

// --- 混合因子 ---
enum class BlendFactor : u8 {
    Zero,             // 0
    One,              // 1
    SrcColor,         // 源颜色
    OneMinusSrcColor, // 1 - 源颜色
    SrcAlpha,         // 源 Alpha
    OneMinusSrcAlpha, // 1 - 源 Alpha
    DstColor,         // 目标颜色
    OneMinusDstColor, // 1 - 目标颜色
    DstAlpha,         // 目标 Alpha
    OneMinusDstAlpha, // 1 - 目标 Alpha
};

// --- 混合操作 ---
enum class BlendOp : u8 {
    Add,             // 加法
    Subtract,        // 减法（源 - 目标）
    ReverseSubtract, // 反向减法（目标 - 源）
    Min,             // 最小值
    Max,             // 最大值
};

// --- 颜色写入掩码 ---
enum class ColorWriteMask : u8 {
    None  = 0,
    Red   = 1 << 0,
    Green = 1 << 1,
    Blue  = 1 << 2,
    Alpha = 1 << 3,
    All   = Red | Green | Blue | Alpha,
};
inline ColorWriteMask operator|(ColorWriteMask a, ColorWriteMask b) { return ColorWriteMask(u8(a) | u8(b)); }

// --- 混合状态（每 MRT） ---
struct ColorBlendDesc {
    bool        blendEnable         = false;
    BlendFactor srcColorBlendFactor = BlendFactor::One;
    BlendFactor dstColorBlendFactor = BlendFactor::Zero;
    BlendOp     colorBlendOp        = BlendOp::Add;
    BlendFactor srcAlphaBlendFactor = BlendFactor::One;
    BlendFactor dstAlphaBlendFactor = BlendFactor::Zero;
    BlendOp     alphaBlendOp        = BlendOp::Add;
    ColorWriteMask writeMask        = ColorWriteMask::All;
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
    // 异步计算支持
    bool    supportsAsyncCompute   = false;  // GPU 是否有独立 Compute 队列族
    bool    supportsTransferQueue  = false;  // GPU 是否有独立 Copy 队列 (DMA)

    // Device Generated Commands 支持（VK_EXT_device_generated_commands）
    bool    supportsDGC            = false;  // DGC 硬件加速
    u32     asyncComputeTier       = 0;      // 0=不支持, 1=独立队列族, 2=专用硬件引擎

    // Ray Tracing 详细能力（supportsRayTracing=true 时有效）
    u32     maxRayRecursionDepth     = 1;     // 最大递归深度
    u32     shaderGroupHandleSize    = 32;    // 着色器组句柄大小（字节）
    u32     shaderGroupBaseAlignment = 64;    // 着色器组句柄对齐（字节）
    u64     maxRTDispatchSize        = 0;     // 单次 TraceRays 最大尺寸 (width*height*depth)
    u64     maxASInstanceCount       = 0;     // TLAS 最大实例数
    u64     maxASGeometryCount       = 0;     // BLAS 最大几何数
    u64     maxASPrimitiveCount      = 0;     // BLAS 最大三角形数
    u64     minASScratchAlignment    = 0;     // Scratch 缓冲区最小对齐（字节）

    // Mesh Shader 详细能力（supportsMeshShaders=true 时有效）
    u32     maxMeshWorkGroupInvocations = 128;  // Mesh Shader 单工作组最大调用数
    u32     maxMeshOutputVertices       = 256;  // Mesh Shader 最大输出顶点数
    u32     maxMeshOutputPrimitives     = 256;  // Mesh Shader 最大输出图元数
    u32     maxTaskWorkGroupInvocations = 128;  // Task Shader 单工作组最大调用数
    u32     maxTaskPayloadSize          = 16384; // Task→Mesh payload 最大字节数
    u32     maxMeshWorkGroupCountX      = 65535; // Mesh 工作组 X 方向最大数
    u32     maxMeshWorkGroupCountY      = 65535; // Mesh 工作组 Y 方向最大数
    u32     maxMeshWorkGroupCountZ      = 65535; // Mesh 工作组 Z 方向最大数
};

// --- 渲染通道加载操作 ---
enum class LoadOp : u8 { Clear = 0, Load = 1 };  // Clear=清屏, Load=保留内容

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
    SampledImage,               // 采样图像（Texture2D 等，不含采样器）
    Sampler,                    // 独立采样器
    InputAttachment,            // 输入附件（用于 SubPass）
    AccelerationStructure,      // 加速结构（用于 RT PSO 描述符集，绑定 TLAS）
};

// --- 描述符集布局 ---
struct DescriptorSetLayoutBinding {
    u32             binding     = 0;        // binding 编号
    DescriptorType  type        = DescriptorType::UniformBuffer;
    u32             count       = 1;        // 数组元素数
    u32             stageMask   = kStageMaskVertex;  // Shader Stage 位掩码（kStageMask* 常量）
    bool            bindless    = false;    // 是否为无绑定数组
};

struct DescriptorSetLayoutDesc {
    std::vector<DescriptorSetLayoutBinding> bindings;
};

// --- 描述符句柄（不透明，Vulkan 内部实现）---
using DescriptorSetLayoutHandle = u64;
using DescriptorSetHandle        = u64;
constexpr DescriptorSetLayoutHandle kInvalidLayout = 0;
constexpr DescriptorSetHandle       kInvalidSet    = 0;

// --- 跨队列同步原语（不透明句柄）---
// Vulkan: 封装 VkSemaphore (Timeline Semaphore)
// D3D12:  封装 ID3D12Fence
using RHIFenceHandle            = u64;
constexpr RHIFenceHandle kInvalidFence = 0;

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
    RayTracingShader            = 1 << 11,  // VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR
    AccelerationStructureBuild  = 1 << 12,  // VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
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
