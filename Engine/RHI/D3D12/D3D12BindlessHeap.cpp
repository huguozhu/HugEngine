// ============================================================================
//  D3D12BindlessHeap.cpp
// ============================================================================
#include "D3D12BindlessHeap.h"
#include "D3D12Device.h"

#if defined(_WIN32)

namespace he::rhi::d3d12 {

BindlessHandle D3D12BindlessHeap::RegisterTexture(IRHITexture* texture, IRHISampler* sampler) {
    if (m_Textures.empty()) m_Textures.emplace_back();   // 槽位 0 = 默认
    Entry e;
    e.tex = texture;
    e.smp = sampler;
    m_Textures.push_back(e);
    return static_cast<BindlessHandle>(m_Textures.size() - 1);
}

BindlessHandle D3D12BindlessHeap::RegisterSampler(IRHISampler* sampler) {
    m_Samplers.push_back(sampler);
    return static_cast<BindlessHandle>(m_Samplers.size() - 1);
}

BindlessHandle D3D12BindlessHeap::RegisterBuffer(IRHIBuffer* ssbo) {
    // 槽位 0 保留为无效哨兵（引擎里有 `handle == 0 表示无效` 的约定）
    if (m_Buffers.empty()) m_Buffers.push_back(nullptr);
    m_Buffers.push_back(ssbo);
    return static_cast<BindlessHandle>(m_Buffers.size() - 1);
}

void D3D12BindlessHeap::SetDefaultTexture(IRHITexture* texture, IRHISampler* sampler) {
    if (m_Textures.empty()) m_Textures.emplace_back();
    m_Textures[0].tex = texture;
    m_Textures[0].smp = sampler;
}

} // namespace he::rhi::d3d12

#endif // _WIN32
