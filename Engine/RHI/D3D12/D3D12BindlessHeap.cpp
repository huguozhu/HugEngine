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
    const BindlessHandle h = static_cast<BindlessHandle>(m_Textures.size() - 1);
    // 着色器里 bindless 纹理/采样器数组在 D3D12 下是 1024 个（见 common.slang），超出会采样到别的资源
    if (h >= 1024u) {
        static bool s_warned = false;
        if (!s_warned) { s_warned = true; HE_CORE_ERROR("D3D12BindlessHeap: bindless 纹理超过 1024 个，超出部分着色器无法访问"); }
    }
    // 关键：真正把 SRV 写进 shader-visible 堆，否则着色器按句柄索引到的是空描述符
    m_Device->WriteBindlessSrv(static_cast<u32>(h), texture);
    m_Device->WriteBindlessSampler(static_cast<u32>(h), sampler);   // 纹理与采样器同槽位配对
    return h;
}

BindlessHandle D3D12BindlessHeap::RegisterSampler(IRHISampler* sampler) {
    m_Samplers.push_back(sampler);
    return static_cast<BindlessHandle>(m_Samplers.size() - 1);
}

BindlessHandle D3D12BindlessHeap::RegisterBuffer(IRHIBuffer* ssbo) {
    // 槽位 0 保留为无效哨兵（引擎里有 `handle == 0 表示无效` 的约定）
    if (m_Buffers.empty()) m_Buffers.push_back(nullptr);
    m_Buffers.push_back(ssbo);
    const BindlessHandle h = static_cast<BindlessHandle>(m_Buffers.size() - 1);
    // 【多区间写入】D3D12 不允许同一 space 内多个无界数组，所以着色器里每个 bindless
    // SSBO 数组各占一段定长区间：u_InstanceTransforms=t1024、u_VisibleInstanceIndices=t1088、
    // u_BoneMatrices=t1152、u_Materials=t1216（每段 64 个槽；t0..t1023 留给 bindless 纹理）。
    // 同一个缓冲注册到统一句柄空间，因此要把它的描述符写进全部四段，
    // 这样任何数组按 handle 索引都能取到同一份数据。
    const u32 hIdx = static_cast<u32>(h);
    for (u32 base : { 1024u, 1088u, 1152u, 1216u }) {
        m_Device->WriteBindlessBufferSrv(base + hIdx, ssbo);
        // 【索引 0 也要写】着色器里材质缓冲的用法是 `u_Materials[0][materialID>>2]`
        // —— 数组下标 0 表示"材质缓冲本身"（引擎约定），而堆内 0 号被保留为无效哨兵。
        // 只写 base+h 会让 `u_Materials[0]` 永远取到空描述符 ⇒ 材质全 0 ⇒ alpha=0
        // ⇒ 像素着色器把所有片元 discard 掉（实测画面全黑的直接原因）。
        // 这里把第一个注册的缓冲（材质缓冲）同时写到索引 0。
        // 材质缓冲的元素大小是 sizeof(GPUMaterialData)=112，用它唯一识别材质缓冲
        // （实例变换 400000、实例数据 640000、剔除命令 1536 都不能被 112 整除）。
        if (base == 1216u && (ssbo->GetSize() % 112ull) == 0ull && ssbo->GetSize() > 0ull) {
            m_Device->WriteBindlessBufferSrv(base, ssbo);
        }
    }
    return h;
}

void D3D12BindlessHeap::SetDefaultTexture(IRHITexture* texture, IRHISampler* sampler) {
    if (m_Textures.empty()) m_Textures.emplace_back();
    m_Textures[0].tex = texture;
    m_Textures[0].smp = sampler;
}

} // namespace he::rhi::d3d12

#endif // _WIN32
