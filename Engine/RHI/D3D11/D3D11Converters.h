// ============================================================================
//  D3D11Converters.h —— 引擎枚举 → D3D11 / DXGI 枚举的映射
//
//  纯函数、无状态。纹理格式按「资源 / SRV / RTV / DSV / UAV」分别给出：
//  深度纹理要同时作深度附件与 SRV 时，资源必须用 TYPELESS 格式，视图各取具体格式。
// ============================================================================
#pragma once

#if defined(_WIN32)

#include "D3D11Common.h"

namespace he::rhi::d3d11 {

/// 一种引擎纹理格式在 D3D11 下的各种用法所需的 DXGI 格式
struct TextureFormats {
    DXGI_FORMAT resource = DXGI_FORMAT_UNKNOWN;   // 创建资源用（需要多种视图时为 TYPELESS）
    DXGI_FORMAT srv      = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT rtv      = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT dsv      = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT uav      = DXGI_FORMAT_UNKNOWN;   // sRGB 格式不能作 UAV，取对应的线性格式
};

/// 引擎格式 → 直接对应的 DXGI 格式（交换链、渲染目标描述用）；未知返回 UNKNOWN
DXGI_FORMAT    ToDxgiFormat(Format format);
/// 按用途给出资源与各视图的格式（深度 + SRV、sRGB + UAV 时资源用 TYPELESS）
TextureFormats GetTextureFormats(Format format, TextureUsage usage);
/// 深度 / 模板格式
bool           IsDepthFormat(Format format);
/// 是否带模板分量（清除时决定是否带 D3D11_CLEAR_STENCIL）
bool           HasStencil(Format format);
/// 块压缩格式（行距按 4×4 块计算）
bool           IsBlockCompressed(Format format);
/// 每像素字节数（块压缩格式为每 4×4 块字节数）
u32            FormatByteSize(Format format);

/// 顶点属性格式 → DXGI 格式
DXGI_FORMAT    ToDxgiVertexFormat(VertexFormat format);

D3D11_PRIMITIVE_TOPOLOGY   ToD3D11Topology(PrimitiveTopology topology);
D3D11_FILL_MODE            ToD3D11FillMode(FillMode mode);
D3D11_CULL_MODE            ToD3D11CullMode(CullMode mode);
D3D11_COMPARISON_FUNC      ToD3D11CompareFunc(CompareFunc func);
D3D11_BLEND                ToD3D11Blend(BlendFactor factor);
D3D11_BLEND_OP             ToD3D11BlendOp(BlendOp op);
D3D11_TEXTURE_ADDRESS_MODE ToD3D11AddressMode(AddressMode mode);
/// 由缩小 / 放大 / mip 三种过滤组合出 D3D11 过滤模式；anisotropy / compare 分别选各向异性与比较过滤
D3D11_FILTER               ToD3D11Filter(FilterMode minF, FilterMode magF, FilterMode mipF,
                                         bool anisotropy, bool compare);

} // namespace he::rhi::d3d11

#endif // _WIN32
