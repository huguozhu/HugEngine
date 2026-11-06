// ============================================================================
//  D3D11Converters.cpp —— 引擎枚举 → D3D11 / DXGI 枚举
// ============================================================================
#include "D3D11Converters.h"

namespace he::rhi::d3d11 {

DXGI_FORMAT ToDxgiFormat(Format format) {
    switch (format) {
        // DXGI 没有单 / 双通道 sRGB 格式，退化为 UNORM（与 D3D12 后端一致）
        case Format::R8_UNORM:      case Format::R8_SRGB:  return DXGI_FORMAT_R8_UNORM;
        case Format::RG8_UNORM:     case Format::RG8_SRGB: return DXGI_FORMAT_R8G8_UNORM;
        case Format::RGBA8_UNORM:   return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::RGBA8_SRGB:    return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case Format::BGRA8_UNORM:   return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Format::BGRA8_SRGB:    return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case Format::R16_FLOAT:     return DXGI_FORMAT_R16_FLOAT;
        case Format::RG16_FLOAT:    return DXGI_FORMAT_R16G16_FLOAT;
        case Format::RGBA16_FLOAT:  return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case Format::R32_FLOAT:     return DXGI_FORMAT_R32_FLOAT;
        case Format::RG32_FLOAT:    return DXGI_FORMAT_R32G32_FLOAT;
        case Format::RGB32_FLOAT:   return DXGI_FORMAT_R32G32B32_FLOAT;
        case Format::RGBA32_FLOAT:  return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case Format::R32_UINT:      return DXGI_FORMAT_R32_UINT;
        case Format::R11G11B10_FLOAT:          return DXGI_FORMAT_R11G11B10_FLOAT;
        case Format::A2B10G10R10_UNORM_PACK32: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case Format::D16_UNORM:         return DXGI_FORMAT_D16_UNORM;
        case Format::D32_FLOAT:         return DXGI_FORMAT_D32_FLOAT;
        case Format::D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case Format::D32_FLOAT_S8_UINT: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        case Format::BC1_UNORM: return DXGI_FORMAT_BC1_UNORM;
        case Format::BC3_UNORM: return DXGI_FORMAT_BC3_UNORM;
        case Format::BC4_UNORM: return DXGI_FORMAT_BC4_UNORM;
        case Format::BC5_UNORM: return DXGI_FORMAT_BC5_UNORM;
        case Format::BC7_UNORM: return DXGI_FORMAT_BC7_UNORM;
        default:                return DXGI_FORMAT_UNKNOWN;
    }
}

TextureFormats GetTextureFormats(Format format, TextureUsage usage) {
    TextureFormats f;
    const DXGI_FORMAT direct = ToDxgiFormat(format);
    f.resource = f.srv = f.rtv = f.uav = direct;

    // 深度：资源用 TYPELESS，DSV 用深度格式，SRV 用对应的颜色格式（深度纹理要被采样时必须这样建）
    switch (format) {
        case Format::D16_UNORM:
            f.resource = DXGI_FORMAT_R16_TYPELESS; f.dsv = DXGI_FORMAT_D16_UNORM; f.srv = DXGI_FORMAT_R16_UNORM;
            f.rtv = f.uav = DXGI_FORMAT_UNKNOWN;
            return f;
        case Format::D32_FLOAT:
            f.resource = DXGI_FORMAT_R32_TYPELESS; f.dsv = DXGI_FORMAT_D32_FLOAT; f.srv = DXGI_FORMAT_R32_FLOAT;
            // D32 纹理作存储图像（Nanite 软光栅写深度）时用 R32_FLOAT 的 UAV
            f.rtv = DXGI_FORMAT_UNKNOWN; f.uav = DXGI_FORMAT_R32_FLOAT;
            return f;
        case Format::D24_UNORM_S8_UINT:
            f.resource = DXGI_FORMAT_R24G8_TYPELESS; f.dsv = DXGI_FORMAT_D24_UNORM_S8_UINT;
            f.srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; f.rtv = f.uav = DXGI_FORMAT_UNKNOWN;
            return f;
        case Format::D32_FLOAT_S8_UINT:
            f.resource = DXGI_FORMAT_R32G8X24_TYPELESS; f.dsv = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            f.srv = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; f.rtv = f.uav = DXGI_FORMAT_UNKNOWN;
            return f;
        default: break;
    }

    // sRGB 不能作 UAV：需要 UAV 时资源用 TYPELESS，UAV 取线性格式、其余视图保持 sRGB
    if (HasAny(usage, TextureUsage::UnorderedAccess)) {
        if (format == Format::RGBA8_SRGB) {
            f.resource = DXGI_FORMAT_R8G8B8A8_TYPELESS; f.uav = DXGI_FORMAT_R8G8B8A8_UNORM;
        } else if (format == Format::BGRA8_SRGB) {
            f.resource = DXGI_FORMAT_B8G8R8A8_TYPELESS; f.uav = DXGI_FORMAT_B8G8R8A8_UNORM;
        }
    }
    return f;
}

bool IsDepthFormat(Format format) {
    return format == Format::D16_UNORM || format == Format::D32_FLOAT ||
           format == Format::D24_UNORM_S8_UINT || format == Format::D32_FLOAT_S8_UINT;
}

bool HasStencil(Format format) {
    return format == Format::D24_UNORM_S8_UINT || format == Format::D32_FLOAT_S8_UINT;
}

bool IsBlockCompressed(Format format) {
    return format == Format::BC1_UNORM || format == Format::BC3_UNORM || format == Format::BC4_UNORM ||
           format == Format::BC5_UNORM || format == Format::BC7_UNORM;
}

u32 FormatByteSize(Format format) {
    switch (format) {
        case Format::R8_UNORM:  case Format::R8_SRGB:  return 1;
        case Format::RG8_UNORM: case Format::RG8_SRGB: case Format::R16_FLOAT: case Format::D16_UNORM: return 2;
        case Format::RGBA8_UNORM: case Format::RGBA8_SRGB: case Format::BGRA8_UNORM: case Format::BGRA8_SRGB:
        case Format::RG16_FLOAT: case Format::R32_FLOAT: case Format::R32_UINT: case Format::R11G11B10_FLOAT:
        case Format::A2B10G10R10_UNORM_PACK32: case Format::D32_FLOAT: case Format::D24_UNORM_S8_UINT: return 4;
        case Format::RGBA16_FLOAT: case Format::RG32_FLOAT: case Format::D32_FLOAT_S8_UINT: return 8;
        case Format::RGB32_FLOAT:  return 12;
        case Format::RGBA32_FLOAT: return 16;
        // 块压缩：每 4×4 块的字节数
        case Format::BC1_UNORM: case Format::BC4_UNORM: return 8;
        case Format::BC3_UNORM: case Format::BC5_UNORM: case Format::BC7_UNORM: return 16;
        default: return 4;
    }
}

DXGI_FORMAT ToDxgiVertexFormat(VertexFormat format) {
    switch (format) {
        case VertexFormat::Float:       return DXGI_FORMAT_R32_FLOAT;
        case VertexFormat::Float2:      return DXGI_FORMAT_R32G32_FLOAT;
        case VertexFormat::Float3:      return DXGI_FORMAT_R32G32B32_FLOAT;
        case VertexFormat::Float4:      return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case VertexFormat::UByte4:      return DXGI_FORMAT_R8G8B8A8_UINT;
        case VertexFormat::UByte4_Norm: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case VertexFormat::Byte4_Norm:  return DXGI_FORMAT_R8G8B8A8_SNORM;
        case VertexFormat::UInt:        return DXGI_FORMAT_R32_UINT;
        case VertexFormat::UInt2:       return DXGI_FORMAT_R32G32_UINT;
        case VertexFormat::UInt4:       return DXGI_FORMAT_R32G32B32A32_UINT;
    }
    return DXGI_FORMAT_UNKNOWN;
}

D3D11_PRIMITIVE_TOPOLOGY ToD3D11Topology(PrimitiveTopology topology) {
    switch (topology) {
        case PrimitiveTopology::TriangleList:  return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case PrimitiveTopology::TriangleStrip: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case PrimitiveTopology::LineList:      return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        case PrimitiveTopology::PointList:     return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    }
    return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

D3D11_FILL_MODE ToD3D11FillMode(FillMode mode) {
    return mode == FillMode::Wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
}

D3D11_CULL_MODE ToD3D11CullMode(CullMode mode) {
    switch (mode) {
        case CullMode::Front: return D3D11_CULL_FRONT;
        case CullMode::Back:  return D3D11_CULL_BACK;
        // D3D11 没有「正反面都剔除」：该模式只用于点 / 线，退化为不剔除
        default:              return D3D11_CULL_NONE;
    }
}

D3D11_COMPARISON_FUNC ToD3D11CompareFunc(CompareFunc func) {
    switch (func) {
        case CompareFunc::Never:        return D3D11_COMPARISON_NEVER;
        case CompareFunc::Less:         return D3D11_COMPARISON_LESS;
        case CompareFunc::Equal:        return D3D11_COMPARISON_EQUAL;
        case CompareFunc::LessEqual:    return D3D11_COMPARISON_LESS_EQUAL;
        case CompareFunc::Greater:      return D3D11_COMPARISON_GREATER;
        case CompareFunc::NotEqual:     return D3D11_COMPARISON_NOT_EQUAL;
        case CompareFunc::GreaterEqual: return D3D11_COMPARISON_GREATER_EQUAL;
        case CompareFunc::Always:       return D3D11_COMPARISON_ALWAYS;
    }
    return D3D11_COMPARISON_LESS_EQUAL;
}

D3D11_BLEND ToD3D11Blend(BlendFactor factor) {
    switch (factor) {
        case BlendFactor::Zero:             return D3D11_BLEND_ZERO;
        case BlendFactor::One:              return D3D11_BLEND_ONE;
        case BlendFactor::SrcColor:         return D3D11_BLEND_SRC_COLOR;
        case BlendFactor::OneMinusSrcColor: return D3D11_BLEND_INV_SRC_COLOR;
        case BlendFactor::SrcAlpha:         return D3D11_BLEND_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return D3D11_BLEND_INV_SRC_ALPHA;
        case BlendFactor::DstColor:         return D3D11_BLEND_DEST_COLOR;
        case BlendFactor::OneMinusDstColor: return D3D11_BLEND_INV_DEST_COLOR;
        case BlendFactor::DstAlpha:         return D3D11_BLEND_DEST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return D3D11_BLEND_INV_DEST_ALPHA;
    }
    return D3D11_BLEND_ONE;
}

D3D11_BLEND_OP ToD3D11BlendOp(BlendOp op) {
    switch (op) {
        case BlendOp::Add:             return D3D11_BLEND_OP_ADD;
        case BlendOp::Subtract:        return D3D11_BLEND_OP_SUBTRACT;
        case BlendOp::ReverseSubtract: return D3D11_BLEND_OP_REV_SUBTRACT;
        case BlendOp::Min:             return D3D11_BLEND_OP_MIN;
        case BlendOp::Max:             return D3D11_BLEND_OP_MAX;
    }
    return D3D11_BLEND_OP_ADD;
}

D3D11_TEXTURE_ADDRESS_MODE ToD3D11AddressMode(AddressMode mode) {
    switch (mode) {
        case AddressMode::Repeat:         return D3D11_TEXTURE_ADDRESS_WRAP;
        case AddressMode::MirroredRepeat: return D3D11_TEXTURE_ADDRESS_MIRROR;
        case AddressMode::ClampToEdge:    return D3D11_TEXTURE_ADDRESS_CLAMP;
        case AddressMode::ClampToBorder:  return D3D11_TEXTURE_ADDRESS_BORDER;
    }
    return D3D11_TEXTURE_ADDRESS_WRAP;
}

D3D11_FILTER ToD3D11Filter(FilterMode minF, FilterMode magF, FilterMode mipF, bool anisotropy, bool compare) {
    if (anisotropy) return compare ? D3D11_FILTER_COMPARISON_ANISOTROPIC : D3D11_FILTER_ANISOTROPIC;
    // D3D11_ENCODE_BASIC_FILTER 的位布局：min 在第 4 位、mag 在第 2 位、mip 在第 0 位（1 = 线性）
    const UINT bits = (minF == FilterMode::Linear ? 0x10u : 0u) |
                      (magF == FilterMode::Linear ? 0x04u : 0u) |
                      (mipF == FilterMode::Linear ? 0x01u : 0u);
    return static_cast<D3D11_FILTER>(bits | (compare ? 0x80u : 0u));
}

} // namespace he::rhi::d3d11
