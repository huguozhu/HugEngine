// ============================================================================
//  D3D12RayTracing.h —— D3D12（DXR）光线追踪对象
//
//  对应 Vulkan 侧 Engine/RHI/Vulkan/VulkanRT.cpp 的两类对象：
//    · D3D12AccelerationStructure       ≈ VulkanAccelerationStructure（BLAS / TLAS）
//    · D3D12RayTracingPipelineState     ≈ VulkanRTPipelineState（ID3D12StateObject）
//
//  与 Vulkan 的主要差异（详见 docs/计划实现功能/D3D12剩余任务实施指南.md T5）：
//    · D3D12 的加速结构就是一块 **DEFAULT 堆上的缓冲**（带 ALLOW_UNORDERED_ACCESS），
//      没有独立的 VkAccelerationStructureKHR 对象；它**永远**处于
//      D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE 状态，
//      构建前后只发 UAV 屏障、不发状态转换。
//    · RT 管线是一个状态对象（ID3D12StateObject），着色器以 DXIL library 形式
//      整体导出，组句柄由 ID3D12StateObjectProperties 按导出名取回（每组 32 字节）。
//    · 全局根签名与图形 / 计算管线共用同一套布局（CreateEngineRootSignature）。
// ============================================================================
#pragma once

#if defined(_WIN32)

#include "D3D12Common.h"
#include "RHI/RayTracing.h"

#include <vector>

namespace he::rhi::d3d12 {

class D3D12Device;

// ---------------------------------------------------------------------------
//  公共工具
// ---------------------------------------------------------------------------

/// 创建引擎统一的根签名（图形 / 计算 / 光追三处共用）：
///   0..7   root CBV（b0..b7，推送常量走常量环）
///   8..15  SRV 表（space 0..7，附带 b8.. 的 CBV 范围）
///   16..23 UAV 表（space 0..7）
///   24..31 采样器表（space 0..7）
/// 参数布局必须与 D3D12CommandList 里的根参数编号（SetRootTable）严格一致。
bool CreateEngineRootSignature(ID3D12Device* device, ID3D12RootSignature** outRootSig);

/// 引擎 AS 构建标志 → D3D12 构建标志
D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS ToD3D12ASBuildFlags(ASBuildFlags flags);

/// 引擎几何描述（顶点 / 索引缓冲）→ D3D12 三角形几何描述。
/// 缓冲的 GPU 地址直接取自 IRHIBuffer::GetDeviceAddress()（UPLOAD / DEFAULT 堆均可）。
void FillD3D12GeometryDesc(const RTGeometryDesc& src, D3D12_RAYTRACING_GEOMETRY_DESC& dst);

// ---------------------------------------------------------------------------
//  加速结构（BLAS / TLAS）
// ---------------------------------------------------------------------------
class D3D12AccelerationStructure final : public IRHIAccelerationStructure {
public:
    D3D12AccelerationStructure() = default;
    ~D3D12AccelerationStructure() override = default;

    /// 创建底层存储：DEFAULT 堆 + ALLOW_UNORDERED_ACCESS，初始状态即
    /// RAYTRACING_ACCELERATION_STRUCTURE（此后不再做状态转换）
    bool Initialize(D3D12Device* device, AccelerationStructureType type, u64 size);

    u64 GetDeviceAddress() const override { return m_Address; }
    u64 GetSize() const override { return m_Size; }

    ID3D12Resource*            GetResource() const { return m_Resource.Get(); }
    AccelerationStructureType  GetType()     const { return m_Type; }

private:
    ComPtr<ID3D12Resource>    m_Resource;
    u64                       m_Address = 0;      // 资源起点 GPU 地址（构建 / 绑定都用它）
    u64                       m_Size    = 0;
    AccelerationStructureType m_Type    = AccelerationStructureType::BottomLevel;
};

// ---------------------------------------------------------------------------
//  RT 管线状态（ID3D12StateObject）
// ---------------------------------------------------------------------------
class D3D12RayTracingPipelineState final : public IRHIRayTracingPipelineState {
public:
    D3D12RayTracingPipelineState() = default;
    ~D3D12RayTracingPipelineState() override = default;

    /// 组装并创建光追状态对象：
    ///   DXIL 库子对象（每个 shader 一个，导出名 S&lt;i&gt;）→ 命中组 H&lt;k&gt;
    ///   → 着色器配置 / 管线配置 → 全局根签名
    bool Initialize(D3D12Device* device, const RTPipelineStateDesc& desc);

    u32              GetShaderGroupCount() const override { return m_GroupCount; }
    u32              GetShaderGroupHandleSize() const override { return m_HandleSize; }
    std::vector<u8>  GetShaderGroupHandles() const override { return m_Handles; }

    ID3D12StateObject*          GetStateObject() const { return m_StateObject.Get(); }
    ID3D12RootSignature*        GetGlobalRootSignature() const { return m_RootSig.Get(); }

    /// 着色器实际使用的描述符集资源（来自 ID3D12LibraryReflection）：TraceRays 前据此把
    /// 绑定的纹理 / 缓冲转到所需状态（与图形 / 计算 PSO 的 ResourceUse 同一口径）。
    /// 反射不可用（dxc 拒绝为库出反射）时为空，命令列表回退到按描述符类型保守转换。
    using ResourceUse = D3D12ResourceUse;
    const std::vector<ResourceUse>& GetResourceUses() const { return m_ResourceUses; }

private:
    ComPtr<ID3D12StateObject>           m_StateObject;
    ComPtr<ID3D12StateObjectProperties> m_Properties;
    ComPtr<ID3D12RootSignature>         m_RootSig;
    u32                                 m_GroupCount = 0;
    u32                                 m_HandleSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    std::vector<u8>                     m_Handles;      // 按 shaderGroups 顺序，每组 32 字节
    std::vector<ResourceUse>            m_ResourceUses;
};

} // namespace he::rhi::d3d12

#endif // _WIN32
