#pragma once

#include "RHI/Types.h"
#include "RHI/Buffer.h"
#include "RHI/Shader.h"
#include "Core/Types.h"
#include "Math/Math.h"

#include <vector>

// ============================================================
// RHI Ray Tracing — Acceleration Structure + RT Pipeline State + SBT
// ============================================================

namespace he::rhi {

// ============================================================
// Acceleration Structure 相关枚举与结构体
// ============================================================

// --- 加速结构类型 ---
enum class AccelerationStructureType : u8 {
    BottomLevel,  // BLAS — 每个 mesh 的几何数据
    TopLevel,     // TLAS — 实例集合（BLAS + transform）
};

// --- AS 构建标志 ---
enum class ASBuildFlags : u8 {
    None              = 0,
    AllowUpdate       = 1 << 0,  // 允许增量更新（需要更多内存）
    AllowCompaction   = 1 << 1,  // 允许压缩
    PreferFastTrace   = 1 << 2,  // 优化追踪性能
    PreferFastBuild   = 1 << 3,  // 优化构建速度
    MinimizeMemory    = 1 << 4,  // 最小化内存占用
};
inline ASBuildFlags operator|(ASBuildFlags a, ASBuildFlags b) { return ASBuildFlags(u8(a) | u8(b)); }
inline ASBuildFlags operator&(ASBuildFlags a, ASBuildFlags b) { return ASBuildFlags(u8(a) & u8(b)); }
inline bool          operator!(ASBuildFlags a)                 { return u8(a) == 0; }

// --- 几何类型 ---
enum class RTGeometryType : u8 {
    Triangles,   // 三角形几何（顶点+索引缓冲）
    AABBs,       // 包围盒（用于程序化几何）
    Instances,   // TLAS 实例
};

// --- 单个几何描述（BLAS 构建输入）---
struct RTGeometryDesc {
    RTGeometryType  type              = RTGeometryType::Triangles;
    IRHIBuffer*     vertexBuffer      = nullptr;
    Format          vertexFormat      = Format::RGB32_FLOAT;
    u64             vertexStride      = 12;       // 顶点字节步长
    u32             maxVertex         = 0;        // 最大顶点数
    IRHIBuffer*     indexBuffer       = nullptr;
    Format          indexFormat       = Format::R32_UINT;
    u32             maxPrimitiveCount = 0;        // 最大图元数
    IRHIBuffer*     transformBuffer   = nullptr;  // 4x3 行主序变换矩阵（可选）
};

// --- BLAS 构建描述 ---
struct BLASBuildDesc {
    std::vector<RTGeometryDesc> geometries;
    ASBuildFlags flags          = ASBuildFlags::PreferFastTrace;
    u32          maxVertexCount = 0;
};

// --- TLAS 实例描述（与 Vulkan VkAccelerationStructureInstanceKHR 对应）---
struct TLASInstanceDesc {
    float3x4 transform;                      // 3x4 行主序仿射变换矩阵
    u32      instanceID                 : 24; // 实例自定义 ID（gl_InstanceCustomIndexEXT）
    u32      instanceMask               : 8;  // 实例可见性掩码
    u32      sbtOffset                  : 24; // Shader Binding Table 偏移（命中组索引）
    u32      flags                      : 8;  // VkGeometryInstanceFlagsKHR
    u64      blasAddress;                     // BLAS GPU 地址
};

// --- TLAS 构建描述 ---
struct TLASBuildDesc {
    u32          maxInstanceCount = 0;
    ASBuildFlags flags            = ASBuildFlags::PreferFastTrace;
};

// --- AS 构建所需缓冲区大小 ---
struct ASBuildSizes {
    u64 accelerationStructureSize = 0;  // AS 本身所需内存
    u64 buildScratchSize          = 0;  // 构建临时缓冲区大小
    u64 updateScratchSize         = 0;  // 更新临时缓冲区大小
};

// --- 加速结构抽象接口 ---
class IRHIAccelerationStructure {
public:
    virtual ~IRHIAccelerationStructure() = default;
    virtual u64 GetDeviceAddress() const = 0;  // GPU 地址（用于 TLAS 实例引用）
    virtual u64 GetSize() const = 0;           // AS 占用字节数
};

// ============================================================
// RT Pipeline State 相关枚举与结构体
// ============================================================

// --- 着色器组类型 ---
enum class RTShaderGroupType : u8 {
    RayGen,        // 光线生成（每组 1 个，入口着色器）
    Miss,          // 未命中（每组 1 个）
    Hit,           // 命中组（ClosestHit + 可选 AnyHit + 可选 Intersection）
    Callable,      // 可调用着色器
};

// --- 着色器组描述 ---
struct RTShaderGroup {
    RTShaderGroupType type              = RTShaderGroupType::Hit;
    u32               generalShader     = ~0u;  // RayGen / Miss 在 shaders 数组中的索引
    u32               closestHitShader  = ~0u;  // ClosestHit 在 shaders 数组中的索引
    u32               anyHitShader      = ~0u;  // AnyHit 在 shaders 数组中的索引（可选）
    u32               intersectionShader = ~0u; // Intersection 在 shaders 数组中的索引（可选，默认使用 AABB）
    String            name;                      // 调试名称
};

// --- RT Pipeline State 描述 ---
struct RTPipelineStateDesc {
    std::vector<ShaderBytecode> shaders;         // 所有 RT shader 的字节码（flat 数组）
    std::vector<RTShaderGroup>  shaderGroups;    // 着色器组列表

    u32 maxRecursionDepth   = 1;                 // 最大递归深度
    u32 maxPayloadSize      = 32;                // 光线 payload 最大字节数
    u32 maxHitAttributeSize = 8;                 // 命中属性最大字节数

    std::vector<PushConstantRange> pushConstantRanges;            // Push Constant 范围
    std::vector<DescriptorSetLayoutHandle> descriptorSetLayouts;  // 描述符集布局
    String debugName;                                             // 调试名称
};

// --- RT Pipeline State 抽象接口 ---
class IRHIRayTracingPipelineState {
public:
    virtual ~IRHIRayTracingPipelineState() = default;
    virtual u32 GetShaderGroupCount() const = 0;         // 着色器组数量
    virtual u32 GetShaderGroupHandleSize() const = 0;    // 单个着色器组句柄大小（字节）
    virtual std::vector<u8> GetShaderGroupHandles() const = 0;  // 所有着色器组句柄数据（用于填充 SBT）
};

// ============================================================
// Shader Binding Table (SBT)
// ============================================================

// --- SBT 槽位描述 ---
struct SBTSlot {
    u64 handleOffset = 0;  // 着色器组句柄起始偏移（字节）
    u64 dataOffset   = 0;  // 着色器记录数据偏移（字节）
    u64 dataSize     = 0;  // 着色器记录数据大小（字节）
    u64 stride       = 0;  // 每个 SBT 条目的步长（字节）
};

// --- SBT 完整描述 ---
struct SBTDesc {
    SBTSlot     rayGen;     // RayGen 着色器组槽位
    SBTSlot     miss;       // Miss 着色器组槽位
    SBTSlot     hit;        // Hit 着色器组槽位
    SBTSlot     callable;   // Callable 着色器组槽位
    IRHIBuffer* buffer = nullptr;  // SBT 数据所在的 GPU 缓冲区
};

} // namespace he::rhi
