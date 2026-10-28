// ============================================================================
//  D3D12Converters.cpp —— 枚举映射的实现
//
//  设计说明：
//    · D3D12 的格式枚举比 Vulkan 少（没有 3 通道的 RGB32 等），遇到引擎里有但
//      D3D12 没有的格式统一返回 DXGI_FORMAT_UNKNOWN，由调用方决定是替换还是报错。
//    · ResourceState（引擎的 Vulkan 风格布局名）→ D3D12 状态需要区分颜色/深度，
//      因为 D3D12 的 READ/WRITE 状态是按资源类别区分的。
// ============================================================================
#include "D3D12Converters.h"

#if defined(_WIN32)

namespace he::rhi::d3d12 {


DXGI_FORMAT ToDxgiFormat(Format f) {
    switch (f) {
        case Format::Unknown:                 return DXGI_FORMAT_UNKNOWN;

        // --- 8 位颜色 ---
        case Format::R8_UNORM:                return DXGI_FORMAT_R8_UNORM;
        // DXGI 没有 R8/RG8 的 sRGB 变体（只有 B8G8R8A8/R8G8B8A8 等支持 sRGB）
        // ⇒ 单通道 sRGB 退回线性格式，采样精度差异可接受
        case Format::R8_SRGB:                 return DXGI_FORMAT_R8_UNORM;
        case Format::RG8_UNORM:               return DXGI_FORMAT_R8G8_UNORM;
        case Format::RG8_SRGB:                return DXGI_FORMAT_R8G8_UNORM;
        case Format::RGBA8_UNORM:             return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::RGBA8_SRGB:              return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case Format::BGRA8_UNORM:             return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Format::BGRA8_SRGB:              return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

        // --- 16 位浮点 ---
        case Format::R16_FLOAT:               return DXGI_FORMAT_R16_FLOAT;
        case Format::RG16_FLOAT:              return DXGI_FORMAT_R16G16_FLOAT;
        case Format::RGBA16_FLOAT:            return DXGI_FORMAT_R16G16B16A16_FLOAT;

        // --- 32 位浮点 ---
        case Format::R32_FLOAT:               return DXGI_FORMAT_R32_FLOAT;
        case Format::RG32_FLOAT:              return DXGI_FORMAT_R32G32_FLOAT;
        case Format::RGB32_FLOAT:             return DXGI_FORMAT_R32G32B32_FLOAT;
        case Format::RGBA32_FLOAT:            return DXGI_FORMAT_R32G32B32A32_FLOAT;

        // --- 整数 ---
        case Format::R32_UINT:                return DXGI_FORMAT_R32_UINT;

        // --- 打包 ---
        case Format::R11G11B10_FLOAT:         return DXGI_FORMAT_R11G11B10_FLOAT;
        case Format::A2B10G10R10_UNORM_PACK32:return DXGI_FORMAT_R10G10B10A2_UNORM;

        // --- 深度/模板 ---
        case Format::D16_UNORM:               return DXGI_FORMAT_D16_UNORM;
        case Format::D32_FLOAT:               return DXGI_FORMAT_D32_FLOAT;
        case Format::D24_UNORM_S8_UINT:       return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case Format::D32_FLOAT_S8_UINT:       return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;

        // --- BC 压缩 ---
        case Format::BC1_UNORM:               return DXGI_FORMAT_BC1_UNORM;
        case Format::BC3_UNORM:               return DXGI_FORMAT_BC3_UNORM;
        case Format::BC4_UNORM:               return DXGI_FORMAT_BC4_UNORM;

        default:                              return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_FORMAT ToDxgiVertexFormat(VertexFormat f) {
    switch (f) {
        case VertexFormat::Float:        return DXGI_FORMAT_R32_FLOAT;
        case VertexFormat::Float2:       return DXGI_FORMAT_R32G32_FLOAT;
        case VertexFormat::Float3:       return DXGI_FORMAT_R32G32B32_FLOAT;
        case VertexFormat::Float4:       return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case VertexFormat::UByte4:       return DXGI_FORMAT_R8G8B8A8_UINT;
        case VertexFormat::UByte4_Norm:  return DXGI_FORMAT_R8G8B8A8_UNORM;
        case VertexFormat::Byte4_Norm:   return DXGI_FORMAT_R8G8B8A8_SNORM;
        case VertexFormat::UInt:         return DXGI_FORMAT_R32_UINT;
        case VertexFormat::UInt2:        return DXGI_FORMAT_R32G32_UINT;
        case VertexFormat::UInt4:        return DXGI_FORMAT_R32G32B32A32_UINT;
        default:                         return DXGI_FORMAT_R32G32B32_FLOAT;
    }
}

bool IsDepthFormat(Format f) {
    return f == Format::D16_UNORM || f == Format::D32_FLOAT ||
           f == Format::D24_UNORM_S8_UINT || f == Format::D32_FLOAT_S8_UINT;
}

DXGI_FORMAT ToTypelessFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:  return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:  return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_D16_UNORM:            return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_D32_FLOAT:            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:    return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        default:                               return fmt;
    }
}

D3D12_PRIMITIVE_TOPOLOGY ToD3D12Topology(PrimitiveTopology t) {
    switch (t) {
        case PrimitiveTopology::TriangleList:  return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case PrimitiveTopology::TriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case PrimitiveTopology::LineList:      return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        case PrimitiveTopology::PointList:     return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        default:                               return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE ToD3D12TopologyType(PrimitiveTopology t) {
    switch (t) {
        case PrimitiveTopology::TriangleList:
        case PrimitiveTopology::TriangleStrip: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        case PrimitiveTopology::LineList:      return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        case PrimitiveTopology::PointList:     return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        default:                               return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

D3D12_FILL_MODE ToD3D12FillMode(FillMode m) {
    return (m == FillMode::Wireframe) ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
}

D3D12_CULL_MODE ToD3D12CullMode(CullMode m) {
    switch (m) {
        case CullMode::Front: return D3D12_CULL_MODE_FRONT;
        case CullMode::Back:  return D3D12_CULL_MODE_BACK;
        default:              return D3D12_CULL_MODE_NONE;
    }
}

BOOL ToD3D12FrontCCW(FrontFace f) {
    // Vulkan 默认顺时针为正面；D3D12 的 FrontCounterClockwise 语义相反
    return (f == FrontFace::Clockwise) ? FALSE : TRUE;
}

D3D12_COMPARISON_FUNC ToD3D12CompareFunc(CompareFunc f) {
    switch (f) {
        case CompareFunc::Never:        return D3D12_COMPARISON_FUNC_NEVER;
        case CompareFunc::Less:         return D3D12_COMPARISON_FUNC_LESS;
        case CompareFunc::Equal:        return D3D12_COMPARISON_FUNC_EQUAL;
        case CompareFunc::LessEqual:    return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case CompareFunc::Greater:      return D3D12_COMPARISON_FUNC_GREATER;
        case CompareFunc::NotEqual:     return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case CompareFunc::GreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case CompareFunc::Always:       return D3D12_COMPARISON_FUNC_ALWAYS;
        default:                        return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}

D3D12_BLEND ToD3D12Blend(BlendFactor b) {
    switch (b) {
        case BlendFactor::Zero:        return D3D12_BLEND_ZERO;
        case BlendFactor::One:         return D3D12_BLEND_ONE;
        case BlendFactor::SrcColor:    return D3D12_BLEND_SRC_COLOR;
        case BlendFactor::OneMinusSrcColor: return D3D12_BLEND_INV_SRC_COLOR;
        case BlendFactor::SrcAlpha:    return D3D12_BLEND_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
        case BlendFactor::DstAlpha:    return D3D12_BLEND_DEST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
        case BlendFactor::DstColor:    return D3D12_BLEND_DEST_COLOR;
        case BlendFactor::OneMinusDstColor: return D3D12_BLEND_INV_DEST_COLOR;
        default:                       return D3D12_BLEND_ONE;
    }
}

D3D12_BLEND_OP ToD3D12BlendOp(BlendOp op) {
    switch (op) {
        case BlendOp::Add:             return D3D12_BLEND_OP_ADD;
        case BlendOp::Subtract:        return D3D12_BLEND_OP_SUBTRACT;
        case BlendOp::ReverseSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
        case BlendOp::Min:             return D3D12_BLEND_OP_MIN;
        case BlendOp::Max:             return D3D12_BLEND_OP_MAX;
        default:                       return D3D12_BLEND_OP_ADD;
    }
}

D3D12_TEXTURE_ADDRESS_MODE ToD3D12AddressMode(AddressMode m) {
    switch (m) {
        case AddressMode::Repeat:           return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        case AddressMode::MirroredRepeat:   return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case AddressMode::ClampToEdge:      return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case AddressMode::ClampToBorder:    return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        default:                            return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

D3D12_FILTER ToD3D12Filter(FilterMode minF, FilterMode magF, FilterMode mipF,
                           bool anisotropy, bool compare) {
    // 【三种过滤分别组合】此前只看 minFilter：「缩小 Nearest + 放大/mip Linear」这类组合会
    // 退化成全 Point，既无双线性也无 mip 插值 ⇒ 纹理出现强烈摩尔纹（实测 03.Sponza 的窗帘）。
    const D3D12_FILTER_REDUCTION_TYPE reduction =
        compare ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON : D3D12_FILTER_REDUCTION_TYPE_STANDARD;
    if (anisotropy) return D3D12_ENCODE_ANISOTROPIC_FILTER(reduction);
    auto t = [](FilterMode f) {
        return (f == FilterMode::Nearest) ? D3D12_FILTER_TYPE_POINT : D3D12_FILTER_TYPE_LINEAR;
    };
    return D3D12_ENCODE_BASIC_FILTER(t(minF), t(magF), t(mipF), reduction);
}

D3D12_RESOURCE_STATES ToInitialState(bool isDepth, TextureUsage usage) {
    // 深度纹理的初始状态是 DEPTH_WRITE；颜色纹理按用途决定
    if (isDepth) return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    if (HasAny(usage, TextureUsage::RenderTarget)) return D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (HasAny(usage, TextureUsage::DepthStencil)) return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    if (HasAny(usage, TextureUsage::UnorderedAccess)) return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return D3D12_RESOURCE_STATE_COPY_DEST;
}

D3D12_RESOURCE_STATES ToD3D12ResourceState(ResourceState s, bool isDepth) {
    switch (s) {
        case ResourceState::Undefined:
            return D3D12_RESOURCE_STATE_COMMON;
        case ResourceState::Common:
            return D3D12_RESOURCE_STATE_COMMON;
        case ResourceState::VertexAndConstantBuffer:
            return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        case ResourceState::IndexBuffer:
            return D3D12_RESOURCE_STATE_INDEX_BUFFER;
        case ResourceState::RenderTarget:
            return isDepth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                           : D3D12_RESOURCE_STATE_RENDER_TARGET;
        case ResourceState::DepthStencilRead:
            return D3D12_RESOURCE_STATE_DEPTH_READ;
        case ResourceState::DepthStencilWrite:
            return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        case ResourceState::ShaderResource:
            return D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        case ResourceState::UnorderedAccess:
            return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case ResourceState::CopySrc:
            return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case ResourceState::CopyDst:
            return D3D12_RESOURCE_STATE_COPY_DEST;
        case ResourceState::Present:
            return D3D12_RESOURCE_STATE_PRESENT;
        case ResourceState::IndirectArgument:
            return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        default:
            return D3D12_RESOURCE_STATE_COMMON;
    }
}

D3D12_HEAP_TYPE ToD3D12HeapType(BufferUsage usage) {
    // CPU 可写 → 上传堆；GPU 只读 → 默认堆；读回 → 回读堆（由 CPU 访问标志决定）
    if (HasAny(usage, BufferUsage::TransferDst)) return D3D12_HEAP_TYPE_UPLOAD;
    return D3D12_HEAP_TYPE_DEFAULT;
}

D3D12_HEAP_TYPE ToD3D12HeapType(TextureUsage usage) {
    if (HasAny(usage, TextureUsage::TransferDst)) return D3D12_HEAP_TYPE_UPLOAD;
    return D3D12_HEAP_TYPE_DEFAULT;
}

} // namespace he::rhi::d3d12

#endif // _WIN32
