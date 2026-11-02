// ============================================================================
//  D3D11BindlessHeap.cpp —— bindless 堆的纹理数组桶（M6，说明见头文件）
// ============================================================================
#include "D3D11BindlessHeap.h"
#include "D3D11Device.h"

#if defined(_WIN32)

#include <algorithm>

namespace he::rhi::d3d11 {

BindlessHandle D3D11BindlessHeap::RegisterTexture(IRHITexture* texture, IRHISampler* sampler) {
    TextureEntry e;
    e.srv     = SrvOf(texture);
    e.sampler = StateOf(sampler);
    // 只有静态的普通 2D 纹理能拷进桶：渲染目标 / UAV 的内容会在之后变化，深度 / 多重采样 /
    // 立方体 / 3D / 数组纹理的形状对不上 Texture2DArray 的一层
    if (auto* t = static_cast<D3D11Texture*>(texture)) {
        e.bucketable = e.srv && !t->IsDepth() && !t->IsCube() && !t->Is3D() && t->GetArrayLayers() == 1 &&
                       t->GetSampleCount() == 1 &&
                       !HasAny(t->GetUsage(), TextureUsage::RenderTarget | TextureUsage::UnorderedAccess);
    }
    m_Textures.push_back(std::move(e));
    const BindlessHandle h = static_cast<BindlessHandle>(m_Textures.size() - 1);
    m_Map.resize(m_Textures.size(), kTexBucketNone);
    m_MapDirty = true;
    if (m_Textures.back().bucketable) m_PendingBucket.push_back(h);
    return h;
}

ID3D11ShaderResourceView* D3D11BindlessHeap::GetBucketSrv(u32 k) const {
    return k < m_Buckets.size() ? m_Buckets[k].srv.Get() : nullptr;
}

int D3D11BindlessHeap::FindOrCreateBucket(u32 width, u32 height, u32 mips, DXGI_FORMAT resourceFormat,
                                          DXGI_FORMAT srvFormat) {
    for (size_t i = 0; i < m_Buckets.size(); ++i) {
        const Bucket& b = m_Buckets[i];
        if (b.width == width && b.height == height && b.mips == mips &&
            b.resourceFormat == resourceFormat && b.srvFormat == srvFormat) {
            return static_cast<int>(i);
        }
    }
    if (m_Buckets.size() >= kBindingNBTexBucketCount) return -1;
    Bucket b;
    b.width = width; b.height = height; b.mips = mips;
    b.resourceFormat = resourceFormat; b.srvFormat = srvFormat;
    m_Buckets.push_back(b);
    HE_CORE_INFO("[D3D11] 纹理桶 {}：{}x{}，{} 级 mip，格式 {}",
                 m_Buckets.size() - 1, width, height, mips, static_cast<u32>(srvFormat));
    return static_cast<int>(m_Buckets.size() - 1);
}

bool D3D11BindlessHeap::GrowBucket(Bucket& b, u32 layers) {
    if (layers <= b.capacity) return true;
    constexpr u32 kMaxLayers = D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;   // 2048
    if (layers > kMaxLayers) return false;
    u32 cap = std::max(b.capacity * 2u, 16u);
    while (cap < layers) cap *= 2u;
    cap = std::min(cap, kMaxLayers);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = b.width; td.Height = b.height;
    td.MipLevels = b.mips;
    td.ArraySize = cap;
    td.Format    = b.resourceFormat;
    td.SampleDesc = { 1, 0 };
    td.Usage     = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> arr;
    if (!HE_D3D11_CHECK(m_Device->GetDevice()->CreateTexture2D(&td, nullptr, &arr), "CreateTexture2D(纹理桶)")) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = b.srvFormat;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Texture2DArray = { 0, b.mips, 0, cap };
    ComPtr<ID3D11ShaderResourceView> srv;
    if (!HE_D3D11_CHECK(m_Device->GetDevice()->CreateShaderResourceView(arr.Get(), &sd, &srv), "CreateShaderResourceView(纹理桶)")) {
        return false;
    }
    // 旧数组的内容逐层逐 mip 拷过来（子资源号 = mip + 层 × mip 数，新旧数组的 mip 数相同）
    ID3D11DeviceContext4* ctx = m_Device->GetContext();
    for (u32 l = 0; l < b.used; ++l) {
        for (u32 m = 0; m < b.mips; ++m) {
            const UINT sub = D3D11CalcSubresource(m, l, b.mips);
            ctx->CopySubresourceRegion(arr.Get(), sub, 0, 0, 0, b.array.Get(), sub, nullptr);
        }
    }
    b.array = arr;
    b.srv = srv;
    b.capacity = cap;
    return true;
}

void D3D11BindlessHeap::EnsureBuckets() {
    if (!m_Device) return;
    if (m_PendingBucket.empty() && !m_MapDirty) return;
    if (!m_BucketSampler) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD   = D3D11_FLOAT32_MAX;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        HE_D3D11_CHECK(m_Device->GetDevice()->CreateSamplerState(&sd, &m_BucketSampler), "CreateSamplerState(纹理桶)");
    }

    ID3D11DeviceContext4* ctx = m_Device->GetContext();
    for (BindlessHandle h : m_PendingBucket) {
        const TextureEntry& e = m_Textures[h];
        ComPtr<ID3D11Resource> res;
        e.srv->GetResource(&res);
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(res.As(&tex))) continue;
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        e.srv->GetDesc(&vd);

        const int k = FindOrCreateBucket(td.Width, td.Height, td.MipLevels, td.Format, vd.Format);
        if (k < 0) {
            if (!m_WarnedFull) {
                m_WarnedFull = true;
                HE_CORE_WARN("[D3D11] 纹理桶已满 {} 个，之后尺寸 / 格式不同的纹理不分桶（GPU 驱动路径采样到占位）",
                             kBindingNBTexBucketCount);
            }
            continue;
        }
        Bucket& b = m_Buckets[k];
        if (!GrowBucket(b, b.used + 1)) continue;
        const u32 layer = b.used++;
        for (u32 m = 0; m < b.mips; ++m) {
            ctx->CopySubresourceRegion(b.array.Get(), D3D11CalcSubresource(m, layer, b.mips), 0, 0, 0,
                                       tex.Get(), D3D11CalcSubresource(m, 0, td.MipLevels), nullptr);
        }
        m_Map[h] = kTexBucketValid | (static_cast<u32>(k) << 24) | layer;
        m_MapDirty = true;
    }
    m_PendingBucket.clear();
    if (m_MapDirty) UploadMap();
}

void D3D11BindlessHeap::UploadMap() {
    // 容量按 2 的幂增长（至少 256），未用的元素为「未分桶」
    u32 cap = std::max(m_MapCapacity, 256u);
    while (cap < m_Map.size()) cap *= 2u;
    if (cap != m_MapCapacity || !m_MapBuffer) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = cap * 4u;
        bd.Usage     = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 4;
        ComPtr<ID3D11Buffer> buf;
        if (!HE_D3D11_CHECK(m_Device->GetDevice()->CreateBuffer(&bd, nullptr, &buf), "CreateBuffer(纹理桶映射表)")) return;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = cap;
        ComPtr<ID3D11ShaderResourceView> srv;
        if (!HE_D3D11_CHECK(m_Device->GetDevice()->CreateShaderResourceView(buf.Get(), &sd, &srv), "CreateShaderResourceView(纹理桶映射表)")) {
            return;
        }
        m_MapBuffer = buf;
        m_MapSrv = srv;
        m_MapCapacity = cap;
    }
    std::vector<u32> data(m_MapCapacity, kTexBucketNone);
    std::copy(m_Map.begin(), m_Map.end(), data.begin());
    m_Device->GetContext()->UpdateSubresource(m_MapBuffer.Get(), 0, nullptr, data.data(), 0, 0);
    m_MapDirty = false;
}

} // namespace he::rhi::d3d11

#endif // _WIN32
