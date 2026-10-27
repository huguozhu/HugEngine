// ============================================================================
//  D3D12Converters.h —— 引擎枚举 → D3D12/DXGI 枚举的映射
//
//  D3D12 与 Vulkan 的枚举不是一一对应，这里集中做转换，避免在业务代码里散落
//  大量的 switch。所有函数都是纯函数、无状态，可以自由调用。
// ============================================================================
#pragma once

#if defined(_WIN32)

#include "D3D12Common.h"

namespace he::rhi::d3d12 {

/// 引擎的 Usage 枚举是位标志（u32），但没有提供 HasFlag 辅助函数，这里自己写一个。
/// 放在头文件里是因为多个 .cpp 都要用。
template <typename E>
inline bool HasAny(E value, E bit) {
    return (static_cast<u32>(value) & static_cast<u32>(bit)) != 0u;
}

/// 引擎格式 → DXGI 格式。未知格式返回 DXGI_FORMAT_UNKNOWN（调用方需检查）。
DXGI_FORMAT   ToDxgiFormat(Format format);

/// 顶点属性格式 → DXGI 格式（D3D12 的输入布局用 DXGI_FORMAT 描述每个属性）
DXGI_FORMAT   ToDxgiVertexFormat(VertexFormat format);

/// 引擎格式是否属于深度/模板（决定 BindFlags 与资源状态）
bool          IsDepthFormat(Format format);

/// 引擎格式是否 sRGB（D3D12 用 *_SRGB 变体表达，转换时要换回线性变体）
DXGI_FORMAT   ToTypelessFormat(DXGI_FORMAT fmt);

/// 图元拓扑 → D3D12 拓扑
D3D12_PRIMITIVE_TOPOLOGY ToD3D12Topology(PrimitiveTopology topology);

/// 图元拓扑 → 拓扑类型（用于 PSO 的 PrimitiveTopologyType）
D3D12_PRIMITIVE_TOPOLOGY_TYPE ToD3D12TopologyType(PrimitiveTopology topology);

/// 填充模式
D3D12_FILL_MODE      ToD3D12FillMode(FillMode mode);

/// 剔除模式
D3D12_CULL_MODE      ToD3D12CullMode(CullMode mode);

/// 正面朝向
BOOL                 ToD3D12FrontCCW(FrontFace frontFace);

/// 比较函数
D3D12_COMPARISON_FUNC ToD3D12CompareFunc(CompareFunc func);

/// 混合因子
D3D12_BLEND          ToD3D12Blend(BlendFactor factor);

/// 混合运算
D3D12_BLEND_OP       ToD3D12BlendOp(BlendOp op);

/// 纹理寻址模式
D3D12_TEXTURE_ADDRESS_MODE ToD3D12AddressMode(AddressMode mode);

/// 过滤模式
D3D12_FILTER         ToD3D12Filter(FilterMode mode, bool anisotropy);

/// 资源用途 → 资源的初始状态（D3D12 有状态机，创建时必须给初始状态）
D3D12_RESOURCE_STATES ToInitialState(bool isDepth, TextureUsage usage);

/// 引擎的 ResourceState（布局）→ D3D12 资源状态
D3D12_RESOURCE_STATES ToD3D12ResourceState(ResourceState state, bool isDepth);

/// 缓冲区用途 → 堆类型
D3D12_HEAP_TYPE      ToD3D12HeapType(BufferUsage usage);

/// 纹理用途 → 堆类型
D3D12_HEAP_TYPE      ToD3D12HeapType(TextureUsage usage);

} // namespace he::rhi::d3d12

#endif // _WIN32
