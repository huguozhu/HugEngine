// ============================================================================
//  D3D11Resources.cpp —— D3D11 的 Buffer / Texture / Sampler
// ============================================================================
#include "D3D11Resources.h"
#include "D3D11Converters.h"
#include "D3D11Device.h"

#include <algorithm>
#include <cstring>

namespace he::rhi::d3d11 {

static u32 RoundUp(u32 v, u32 a) { return (v + a - 1u) / a * a; }

/// 带类型的缓冲视图（Buffer<uint4> 等）的元素字节数；只覆盖着色器里会出现的格式
static u32 DxgiElementSize(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT: return 16;
        case DXGI_FORMAT_R32G32B32_FLOAT:    case DXGI_FORMAT_R32G32B32_UINT:    case DXGI_FORMAT_R32G32B32_SINT:    return 12;
        case DXGI_FORMAT_R32G32_FLOAT:       case DXGI_FORMAT_R32G32_UINT:       case DXGI_FORMAT_R32G32_SINT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UINT: return 8;
        case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R11G11B10_FLOAT: return 4;
        case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R8G8_UNORM: return 2;
        case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: return 1;
        default: return 4;
    }
}

// ===========================================================================
//  Buffer
// ===========================================================================
D3D11Buffer::~D3D11Buffer() {
    if (m_Device && m_MapCount > 0) m_Device->OnBufferUnmapped(this);
}

bool D3D11Buffer::Initialize(D3D11Device* device, const BufferDesc& desc) {
    m_Device = device;
    m_Usage  = desc.usage;
    m_Size   = desc.size;
    m_Stride = static_cast<u32>(desc.stride);

    const bool vbib = HasAny(m_Usage, BufferUsage::Vertex) || HasAny(m_Usage, BufferUsage::Index);
    // 只作常量缓冲：D3D11 的常量缓冲不能与其它绑定标志混用，混用时另建一份镜像（见 GetConstantBuffer）
    m_ConstantOnly = HasAny(m_Usage, BufferUsage::Uniform) && !vbib &&
                     !HasAny(m_Usage, BufferUsage::Storage) && !HasAny(m_Usage, BufferUsage::Indirect);

    u32 structStride = 0;
    if (desc.structureStride > 0 && HasAny(m_Usage, BufferUsage::Storage)) {
        if (vbib) {
            HE_CORE_WARN("[D3D11] 缓冲同时是顶点 / 索引缓冲与结构化缓冲，D3D11 不允许，忽略 structureStride={}",
                         static_cast<u64>(desc.structureStride));
        } else {
            structStride = static_cast<u32>(desc.structureStride);
        }
    }
    if (!CreateGpuBuffer(structStride, desc.initialData)) return false;
    // 有初始数据：第一次 Map 时要从 GPU 回读才能拿到内容（不另存一份初始数据）
    m_GpuWritten = (desc.initialData != nullptr);
    return true;
}

bool D3D11Buffer::CreateGpuBuffer(u32 structStride, const void* initial) {
    ID3D11Device5* dev = m_Device->GetDevice();
    // 分配大小：至少 16 字节、16 字节对齐（常量缓冲要求）；结构化缓冲必须是步长的整数倍
    u32 alloc = RoundUp(std::max<u32>(static_cast<u32>(m_Size), 16u), 16u);
    if (structStride > 0) alloc = RoundUp(alloc, structStride);

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = alloc;
    bd.Usage     = D3D11_USAGE_DEFAULT;
    if (m_ConstantOnly) {
        if (alloc > D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16u) {
            HE_CORE_ERROR("[D3D11] 常量缓冲 {} 字节超过 D3D11 上限 64KB", alloc);
            return false;
        }
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    } else {
        if (HasAny(m_Usage, BufferUsage::Vertex))  bd.BindFlags |= D3D11_BIND_VERTEX_BUFFER;
        if (HasAny(m_Usage, BufferUsage::Index))   bd.BindFlags |= D3D11_BIND_INDEX_BUFFER;
        if (HasAny(m_Usage, BufferUsage::Storage)) bd.BindFlags |= D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (structStride > 0) {
            bd.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bd.StructureByteStride = structStride;
        } else if (HasAny(m_Usage, BufferUsage::Storage)) {
            bd.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        }
        // 间接参数缓冲不能同时是结构化缓冲；既是 Storage 又是 Indirect 的缓冲由 M3.5 的影子参数缓冲处理
        if (HasAny(m_Usage, BufferUsage::Indirect) && structStride == 0 && !HasAny(m_Usage, BufferUsage::Storage)) {
            bd.MiscFlags |= D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
        }
    }

    // 初始数据按分配大小补零（D3D11 从 pSysMem 读满 ByteWidth）
    std::vector<u8> padded;
    D3D11_SUBRESOURCE_DATA init{};
    if (initial) {
        padded.assign(alloc, 0);
        std::memcpy(padded.data(), initial, std::min<usize>(m_Size, alloc));
        init.pSysMem = padded.data();
    }
    ComPtr<ID3D11Buffer> buffer;
    if (!HE_D3D11_CHECK(dev->CreateBuffer(&bd, initial ? &init : nullptr, &buffer), "CreateBuffer")) {
        HE_CORE_ERROR("[D3D11] 缓冲创建失败：size={} alloc={} usage=0x{:x} bind=0x{:x} misc=0x{:x} stride={}",
                      static_cast<u64>(m_Size), alloc, static_cast<u32>(m_Usage), bd.BindFlags, bd.MiscFlags, structStride);
        return false;
    }
    m_Buffer       = buffer;
    m_AllocSize    = alloc;
    m_StructStride = structStride;
    m_Staging.Reset();
    m_ConstantMirror.Reset();
    m_Srvs.clear();
    m_Uavs.clear();
    if (!m_Shadow.empty()) m_Shadow.resize(alloc, 0);
    return true;
}

ID3D11Buffer* D3D11Buffer::GetConstantBuffer() {
    if (m_ConstantOnly) return m_Buffer.Get();
    if (!m_ConstantMirror) {
        if (m_AllocSize > D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16u) return nullptr;
        EnsureShadowCurrent();
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = m_AllocSize;
        bd.Usage     = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init{ m_Shadow.data(), 0, 0 };
        HE_D3D11_CHECK(m_Device->GetDevice()->CreateBuffer(&bd, &init, &m_ConstantMirror), "CreateBuffer(常量缓冲镜像)");
    }
    return m_ConstantMirror.Get();
}

void D3D11Buffer::EnsureShadowCurrent() {
    if (m_Shadow.empty()) {
        m_Shadow.assign(m_AllocSize, 0);
        if (m_GpuWritten) ReadbackToShadow();   // 有初始数据或被 GPU 写过：从 GPU 取回内容
        m_Synced = m_Shadow;
        return;
    }
    if (m_GpuWritten) ReadbackToShadow();
}

void D3D11Buffer::ReadbackToShadow() {
    ID3D11DeviceContext4* ctx = m_Device->GetContext();
    if (!m_Staging) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = m_AllocSize;
        bd.Usage          = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        // CopyResource 要求结构化标志与步长一致
        if (m_StructStride > 0) {
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bd.StructureByteStride = m_StructStride;
        }
        if (!HE_D3D11_CHECK(m_Device->GetDevice()->CreateBuffer(&bd, nullptr, &m_Staging), "CreateBuffer(回读)")) return;
    }
    ctx->CopyResource(m_Staging.Get(), m_Buffer.Get());
    D3D11_MAPPED_SUBRESOURCE ms{};
    // 同步回读：Map(READ) 会等 GPU 执行完此前的命令（正确优先，性能问题记录不修）
    if (SUCCEEDED(ctx->Map(m_Staging.Get(), 0, D3D11_MAP_READ, 0, &ms))) {
        std::memcpy(m_Shadow.data(), ms.pData, m_AllocSize);
        ctx->Unmap(m_Staging.Get(), 0);
    }
    m_GpuWritten = false;
    m_Synced = m_Shadow;
}

void D3D11Buffer::UploadShadow() {
    if (m_Shadow.empty()) return;
    ID3D11DeviceContext4* ctx = m_Device->GetContext();
    // UpdateSubresource 在 immediate context 的命令流里排序：之前录的命令读到旧内容，之后的读到新内容
    ctx->UpdateSubresource(m_Buffer.Get(), 0, nullptr, m_Shadow.data(), 0, 0);
    if (m_ConstantMirror) ctx->UpdateSubresource(m_ConstantMirror.Get(), 0, nullptr, m_Shadow.data(), 0, 0);
    m_Synced = m_Shadow;
}

void* D3D11Buffer::Map() {
    if (!m_Buffer) return nullptr;
    EnsureShadowCurrent();
    if (m_MapCount++ == 0) {
        m_Synced = m_Shadow;
        m_Device->OnBufferMapped(this);
    }
    return m_Shadow.data();
}

void D3D11Buffer::Unmap() {
    if (m_MapCount == 0) return;
    if (--m_MapCount == 0) {
        UploadShadow();
        m_Device->OnBufferUnmapped(this);
    }
}

void D3D11Buffer::FlushPersistentWrites() {
    if (m_Shadow.size() == m_Synced.size() &&
        std::memcmp(m_Shadow.data(), m_Synced.data(), m_Shadow.size()) == 0) return;
    UploadShadow();
}

void D3D11Buffer::RefreshPersistentReads() {
    if (!m_GpuWritten) return;
    // 先把 CPU 尚未上传的改动送上去，再回读（否则回读会覆盖 CPU 的改动）
    FlushPersistentWrites();
    ReadbackToShadow();
}

ID3D11ShaderResourceView* D3D11Buffer::GetSrv(ViewKind kind, u32 v) {
    if (!HasAny(m_Usage, BufferUsage::Storage)) {
        if (!m_ReportedConflict) {
            HE_CORE_ERROR("[D3D11] 缓冲（usage=0x{:x}）没有 Storage 用途，不能作着色器资源", static_cast<u32>(m_Usage));
            m_ReportedConflict = true;
        }
        return nullptr;
    }
    if (kind == ViewKind::Structured && m_StructStride != v) {
        // 结构化步长未知或不同：按反射步长重建（顶点 / 索引缓冲不能是结构化缓冲）
        const bool vbib = HasAny(m_Usage, BufferUsage::Vertex) || HasAny(m_Usage, BufferUsage::Index);
        if (vbib || m_StructStride != 0 || v == 0) {
            if (!m_ReportedConflict) {
                HE_CORE_ERROR("[D3D11] 缓冲（usage=0x{:x}）不能以步长 {} 作结构化缓冲（当前步长 {}，顶点/索引缓冲={}）",
                              static_cast<u32>(m_Usage), v, m_StructStride, vbib);
                m_ReportedConflict = true;
            }
            return nullptr;
        }
        EnsureShadowCurrent();
        HE_CORE_INFO("[D3D11] 缓冲 {} 字节按着色器反射步长 {} 重建为结构化缓冲（BufferDesc 未填 structureStride）",
                     static_cast<u64>(m_Size), v);
        const std::vector<u8> content = m_Shadow;
        if (!CreateGpuBuffer(v, content.data())) return nullptr;
        m_GpuWritten = false;
    }
    if (kind != ViewKind::Structured && m_StructStride != 0) {
        if (!m_ReportedConflict) {
            HE_CORE_ERROR("[D3D11] 结构化缓冲（步长 {}）不能再作原始 / 带类型缓冲读取", m_StructStride);
            m_ReportedConflict = true;
        }
        return nullptr;
    }
    const u64 key = ViewKey(kind, v);
    if (auto it = m_Srvs.find(key); it != m_Srvs.end()) return it->second.Get();

    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    switch (kind) {
        case ViewKind::Structured:
            sd.Format = DXGI_FORMAT_UNKNOWN;
            sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            sd.Buffer.FirstElement = 0;
            sd.Buffer.NumElements  = m_AllocSize / v;
            break;
        case ViewKind::Raw:
            sd.Format = DXGI_FORMAT_R32_TYPELESS;
            sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            sd.BufferEx.FirstElement = 0;
            sd.BufferEx.NumElements  = m_AllocSize / 4u;
            sd.BufferEx.Flags        = D3D11_BUFFEREX_SRV_FLAG_RAW;
            break;
        case ViewKind::Typed:
            sd.Format = static_cast<DXGI_FORMAT>(v);
            sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            sd.Buffer.FirstElement = 0;
            sd.Buffer.NumElements  = m_AllocSize / DxgiElementSize(sd.Format);
            break;
    }
    ComPtr<ID3D11ShaderResourceView> srv;
    if (!HE_D3D11_CHECK(m_Device->GetDevice()->CreateShaderResourceView(m_Buffer.Get(), &sd, &srv),
                        "CreateShaderResourceView(Buffer)")) return nullptr;
    return (m_Srvs[key] = srv).Get();
}

ID3D11UnorderedAccessView* D3D11Buffer::GetUav(ViewKind kind, u32 v) {
    // 先走 SRV 路径完成用途检查与结构化重建（两者的约束相同）
    if (!GetSrv(kind, v)) return nullptr;
    const u64 key = ViewKey(kind, v);
    if (auto it = m_Uavs.find(key); it != m_Uavs.end()) return it->second.Get();

    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    switch (kind) {
        case ViewKind::Structured:
            ud.Format = DXGI_FORMAT_UNKNOWN;
            ud.Buffer.NumElements = m_AllocSize / v;
            break;
        case ViewKind::Raw:
            ud.Format = DXGI_FORMAT_R32_TYPELESS;
            ud.Buffer.NumElements = m_AllocSize / 4u;
            ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            break;
        case ViewKind::Typed:
            ud.Format = static_cast<DXGI_FORMAT>(v);
            ud.Buffer.NumElements = m_AllocSize / DxgiElementSize(ud.Format);
            break;
    }
    ComPtr<ID3D11UnorderedAccessView> uav;
    if (!HE_D3D11_CHECK(m_Device->GetDevice()->CreateUnorderedAccessView(m_Buffer.Get(), &ud, &uav),
                        "CreateUnorderedAccessView(Buffer)")) return nullptr;
    return (m_Uavs[key] = uav).Get();
}

// ===========================================================================
//  Texture
// ===========================================================================
D3D11Texture::~D3D11Texture() {
    if (m_Device) m_Device->UnregisterTextureViews(this);
}

bool D3D11Texture::Initialize(D3D11Device* device, const TextureDesc& desc) {
    m_Device      = device;
    m_Width       = std::max(desc.width, 1u);
    m_Height      = std::max(desc.height, 1u);
    m_Depth       = std::max(desc.depth, 1u);
    m_MipLevels   = std::max(desc.mipLevels, 1u);
    m_IsCube      = HasAny(desc.usage, TextureUsage::Cubemap);
    m_ArrayLayers = std::max(desc.arrayLayers, m_IsCube ? 6u : 1u);
    m_SampleCount = std::max(desc.sampleCount, 1u);
    m_Format      = desc.format;
    m_Usage       = desc.usage;
    m_IsDepth     = IsDepthFormat(desc.format);

    const TextureFormats fmts = GetTextureFormats(desc.format, desc.usage);
    m_SrvFormat = fmts.srv; m_RtvFormat = fmts.rtv; m_DsvFormat = fmts.dsv; m_UavFormat = fmts.uav;
    if (fmts.resource == DXGI_FORMAT_UNKNOWN) {
        HE_CORE_ERROR("[D3D11] 不支持的纹理格式 {}", static_cast<u32>(desc.format));
        return false;
    }

    // 绑定标志：一律可采样（引擎常不声明 ShaderResource 就采样渲染目标）
    UINT bind = D3D11_BIND_SHADER_RESOURCE;
    if (m_IsDepth) {
        bind |= D3D11_BIND_DEPTH_STENCIL;
    } else if (HasAny(desc.usage, TextureUsage::RenderTarget)) {
        bind |= D3D11_BIND_RENDER_TARGET;
    }
    if (HasAny(desc.usage, TextureUsage::UnorderedAccess)) {
        if (m_IsDepth || m_SampleCount > 1) {
            // D3D11 不允许深度 / 多重采样纹理同时作 UAV（SupportsStorageImage 对深度格式返回 false）
            HE_CORE_WARN("[D3D11] 深度 / 多重采样纹理不能作 UAV，忽略 UnorderedAccess 用途（格式 {}）",
                         static_cast<u32>(desc.format));
        } else {
            bind |= D3D11_BIND_UNORDERED_ACCESS;
        }
    }

    ID3D11Device5* dev = device->GetDevice();
    HRESULT hr = E_FAIL;
    if (m_Depth > 1) {
        D3D11_TEXTURE3D_DESC td{};
        td.Width = m_Width; td.Height = m_Height; td.Depth = m_Depth;
        td.MipLevels = m_MipLevels;
        td.Format    = fmts.resource;
        td.Usage     = D3D11_USAGE_DEFAULT;
        td.BindFlags = bind & ~UINT(D3D11_BIND_DEPTH_STENCIL);
        ComPtr<ID3D11Texture3D> tex;
        hr = dev->CreateTexture3D(&td, nullptr, &tex);
        m_Resource = tex;
    } else {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = m_Width; td.Height = m_Height;
        td.MipLevels = m_SampleCount > 1 ? 1u : m_MipLevels;
        td.ArraySize = m_ArrayLayers;
        td.Format    = fmts.resource;
        td.SampleDesc = { m_SampleCount, 0 };
        td.Usage     = D3D11_USAGE_DEFAULT;
        td.BindFlags = bind;
        td.MiscFlags = (m_IsCube && m_ArrayLayers % 6 == 0) ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0u;
        ComPtr<ID3D11Texture2D> tex;
        hr = dev->CreateTexture2D(&td, nullptr, &tex);
        m_Resource = tex;
    }
    if (!HE_D3D11_CHECK(hr, "CreateTexture")) {
        HE_CORE_ERROR("[D3D11] 纹理创建失败：{}x{}x{} mips={} layers={} samples={} 格式={} bind=0x{:x}",
                      m_Width, m_Height, m_Depth, m_MipLevels, m_ArrayLayers, m_SampleCount,
                      static_cast<u32>(desc.format), bind);
        return false;
    }

    if (desc.initialData) UploadInitialData(desc);

    // 默认视图：渲染目标 → RTV，深度 → DSV，其余 → 整资源 SRV（见类说明）
    if (m_IsDepth)                              m_DefaultView = GetDsv(0, 0);
    else if (bind & D3D11_BIND_RENDER_TARGET)   m_DefaultView = GetRtv(0, 0);
    else                                        m_DefaultView = GetSrv(0, 0, false);

    // 逐层视图（立方体图逐面渲染、数组层逐层处理）
    if (m_ArrayLayers > 1) {
        m_FaceViews.resize(m_ArrayLayers, nullptr);
        for (u32 l = 0; l < m_ArrayLayers; ++l) {
            if (m_IsDepth)                            m_FaceViews[l] = GetDsv(0, l);
            else if (bind & D3D11_BIND_RENDER_TARGET) m_FaceViews[l] = GetRtv(0, l);
            else                                      m_FaceViews[l] = GetSrv(0, l, true);
        }
    }
    return true;
}

void* D3D11Texture::GetNativeHandle() const { return m_DefaultView; }

void* D3D11Texture::GetNativeHandle(u32 index) const {
    if (m_ArrayLayers <= 1) return index == 0 ? m_DefaultView : nullptr;
    return index < m_FaceViews.size() ? m_FaceViews[index] : nullptr;
}

void D3D11Texture::UploadInitialData(const TextureDesc& desc) {
    // 各数组层的 mip 0 依次紧密排列；3D 纹理是一整块（depth 个切片）
    const bool bc      = IsBlockCompressed(desc.format);
    const u32  texel   = FormatByteSize(desc.format);
    const u32  rowPitch = bc ? ((m_Width + 3u) / 4u) * texel : m_Width * texel;
    const u32  rows     = bc ? (m_Height + 3u) / 4u : m_Height;
    const u32  slicePitch = rowPitch * rows;
    const u8*  src = static_cast<const u8*>(desc.initialData);
    ID3D11DeviceContext4* ctx = m_Device->GetContext();
    const u32 layers = (m_Depth > 1) ? 1u : m_ArrayLayers;
    for (u32 l = 0; l < layers; ++l) {
        const UINT sub = D3D11CalcSubresource(0, l, m_SampleCount > 1 ? 1u : m_MipLevels);
        ctx->UpdateSubresource(m_Resource.Get(), sub, nullptr, src, rowPitch, slicePitch);
        src += static_cast<size_t>(slicePitch) * m_Depth;
    }
}

ID3D11ShaderResourceView* D3D11Texture::GetSrv(u32 mip, u32 layer, bool single) {
    const u64 key = ViewKey(mip, layer, single);
    if (auto it = m_Srvs.find(key); it != m_Srvs.end()) return it->second.Get();

    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = m_SrvFormat;
    const UINT mips = single ? 1u : UINT(-1);
    if (m_Depth > 1) {
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        sd.Texture3D = { mip, mips };
    } else if (m_SampleCount > 1) {
        if (m_ArrayLayers > 1) {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
            sd.Texture2DMSArray = { single ? layer : 0u, single ? 1u : m_ArrayLayers };
        } else {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        }
    } else if (m_IsCube && !single) {
        if (m_ArrayLayers > 6) {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
            sd.TextureCubeArray = { mip, mips, 0, m_ArrayLayers / 6u };
        } else {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            sd.TextureCube = { mip, mips };
        }
    } else if (m_ArrayLayers > 1) {
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray = { mip, mips, single ? layer : 0u, single ? 1u : m_ArrayLayers };
    } else {
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D = { mip, mips };
    }
    ComPtr<ID3D11ShaderResourceView> srv;
    if (!HE_D3D11_CHECK(m_Device->GetDevice()->CreateShaderResourceView(m_Resource.Get(), &sd, &srv),
                        "CreateShaderResourceView(Texture)")) return nullptr;
    m_Device->RegisterView(srv.Get(), { this, D3D11ViewInfo::Type::SRV, mip, layer, single });
    return (m_Srvs[key] = srv).Get();
}

ID3D11UnorderedAccessView* D3D11Texture::GetUav(u32 mip, u32 layer, bool single) {
    const u64 key = ViewKey(mip, layer, single);
    if (auto it = m_Uavs.find(key); it != m_Uavs.end()) return it->second.Get();

    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = m_UavFormat;
    if (m_Depth > 1) {
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
        ud.Texture3D = { mip, 0u, UINT(-1) };
    } else if (m_ArrayLayers > 1) {
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
        ud.Texture2DArray = { mip, single ? layer : 0u, single ? 1u : m_ArrayLayers };
    } else {
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        ud.Texture2D = { mip };
    }
    ComPtr<ID3D11UnorderedAccessView> uav;
    if (!HE_D3D11_CHECK(m_Device->GetDevice()->CreateUnorderedAccessView(m_Resource.Get(), &ud, &uav),
                        "CreateUnorderedAccessView(Texture)")) return nullptr;
    m_Device->RegisterView(uav.Get(), { this, D3D11ViewInfo::Type::UAV, mip, layer, single });
    return (m_Uavs[key] = uav).Get();
}

ID3D11RenderTargetView* D3D11Texture::GetRtv(u32 mip, u32 layer) {
    const u64 key = ViewKey(mip, layer, true);
    if (auto it = m_Rtvs.find(key); it != m_Rtvs.end()) return it->second.Get();

    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = m_RtvFormat;
    if (m_Depth > 1) {
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
        rd.Texture3D = { mip, layer, 1u };
    } else if (m_SampleCount > 1) {
        if (m_ArrayLayers > 1) {
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
            rd.Texture2DMSArray = { layer, 1u };
        } else {
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
        }
    } else if (m_ArrayLayers > 1) {
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rd.Texture2DArray = { mip, layer, 1u };
    } else {
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rd.Texture2D = { mip };
    }
    ComPtr<ID3D11RenderTargetView> rtv;
    if (!HE_D3D11_CHECK(m_Device->GetDevice()->CreateRenderTargetView(m_Resource.Get(), &rd, &rtv),
                        "CreateRenderTargetView")) return nullptr;
    m_Device->RegisterView(rtv.Get(), { this, D3D11ViewInfo::Type::RTV, mip, layer, true });
    return (m_Rtvs[key] = rtv).Get();
}

ID3D11DepthStencilView* D3D11Texture::GetDsv(u32 mip, u32 layer, bool readOnly) {
    const u64 key = ViewKey(mip, layer, true, readOnly);
    if (auto it = m_Dsvs.find(key); it != m_Dsvs.end()) return it->second.Get();

    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = m_DsvFormat;
    // 只读 DSV：同一张深度图在同一 pass 里既作深度测试又被采样时使用（规划 1.3）
    if (readOnly) dd.Flags = D3D11_DSV_READ_ONLY_DEPTH | (HasStencil(m_Format) ? D3D11_DSV_READ_ONLY_STENCIL : 0u);
    if (m_SampleCount > 1) {
        if (m_ArrayLayers > 1) {
            dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
            dd.Texture2DMSArray = { layer, 1u };
        } else {
            dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
        }
    } else if (m_ArrayLayers > 1) {
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dd.Texture2DArray = { mip, layer, 1u };
    } else {
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        dd.Texture2D = { mip };
    }
    ComPtr<ID3D11DepthStencilView> dsv;
    if (!HE_D3D11_CHECK(m_Device->GetDevice()->CreateDepthStencilView(m_Resource.Get(), &dd, &dsv),
                        "CreateDepthStencilView")) return nullptr;
    m_Device->RegisterView(dsv.Get(), { this, D3D11ViewInfo::Type::DSV, mip, layer, true });
    return (m_Dsvs[key] = dsv).Get();
}

// ===========================================================================
//  Sampler
// ===========================================================================
bool D3D11Sampler::Initialize(D3D11Device* device, const SamplerDesc& desc) {
    D3D11_SAMPLER_DESC sd{};
    sd.Filter   = ToD3D11Filter(desc.minFilter, desc.magFilter, desc.mipFilter,
                                desc.maxAnisotropy > 1, desc.enableCompare);
    sd.AddressU = ToD3D11AddressMode(desc.addressU);
    sd.AddressV = ToD3D11AddressMode(desc.addressV);
    sd.AddressW = ToD3D11AddressMode(desc.addressW);
    sd.MipLODBias    = desc.mipLodBias;
    sd.MaxAnisotropy = std::clamp<u32>(desc.maxAnisotropy, 1u, D3D11_REQ_MAXANISOTROPY);
    sd.ComparisonFunc = desc.enableCompare ? ToD3D11CompareFunc(desc.compareFunc) : D3D11_COMPARISON_NEVER;
    sd.MinLOD = desc.minLod;
    sd.MaxLOD = desc.maxLod;
    // 边界色为全 0，与 D3D12 后端一致
    return HE_D3D11_CHECK(device->GetDevice()->CreateSamplerState(&sd, &m_State), "CreateSamplerState");
}

} // namespace he::rhi::d3d11
