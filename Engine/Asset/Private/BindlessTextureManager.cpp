#include "Asset/BindlessTextureManager.h"
#include "Core/Log.h"

// ============================================================
// BindlessTextureManager 实现
// ============================================================

namespace he::asset {

BindlessTextureManager& BindlessTextureManager::Instance() {
    static BindlessTextureManager mgr;
    return mgr;
}

u32 BindlessTextureManager::RegisterTexture(rhi::IRHITexture* texture, rhi::IRHISampler* sampler) {
    // 空纹理/采样器用占位符替代（避免 Vulkan descriptor write 写入空句柄导致验证层报错）
    if (!texture) texture = m_DefaultTexture;
    if (!sampler) sampler = m_DefaultSampler;
    m_Textures.push_back(texture);
    m_Samplers.push_back(sampler);
    u32 idx = m_TextureCount++;
    m_DescDirty = true;
    return idx;
}

u32 BindlessTextureManager::RegisterMaterial(
    rhi::IRHITexture* baseColor, rhi::IRHISampler* bcSamp,
    rhi::IRHITexture* normal,   rhi::IRHISampler* nSamp,
    rhi::IRHITexture* metallicRoughness, rhi::IRHISampler* mrSamp,
    rhi::IRHITexture* occlusion, rhi::IRHISampler* ocSamp)
{
    // 分配 4 个连续纹理索引（baseColor, normal, metallicRoughness, occlusion）
    u32 baseIndex = m_TextureCount;
    RegisterTexture(baseColor, bcSamp);
    RegisterTexture(normal, nSamp);
    RegisterTexture(metallicRoughness, mrSamp);
    RegisterTexture(occlusion, ocSamp);
    return baseIndex;
}

void BindlessTextureManager::UpdateDescriptorSet(
    rhi::IRHIDevice* device, rhi::DescriptorSetHandle set,
    u32 textureBinding, u32 samplerBinding)
{
    if (!m_DescDirty || m_TextureCount == 0) return;

    // 更新纹理数组描述符（SampledImage，仅写入 imageView，sampler 字段忽略）
    std::vector<rhi::IRHITexture*> texPtrs(m_Textures.begin(), m_Textures.begin() + m_TextureCount);
    device->UpdateDescriptorSet(set, textureBinding,
        rhi::DescriptorType::SampledImage,
        texPtrs.data(), nullptr, m_TextureCount);

    // 更新采样器数组描述符（Sampler，仅写入 sampler）
    std::vector<rhi::IRHISampler*> sampPtrs(m_Samplers.begin(), m_Samplers.begin() + m_TextureCount);
    device->UpdateDescriptorSet(set, samplerBinding,
        rhi::DescriptorType::Sampler,
        nullptr, sampPtrs.data(), m_TextureCount);

    // 注意：不在此处清除 m_DescDirty，由调用方在所有需要的描述符集更新完成后调用 ClearDirty()
    // 这样三缓冲等多描述符集场景可以在一次 dirty 周期内更新所有 set
    HE_CORE_INFO("BindlessTextureManager: {} textures updated to descriptor set", m_TextureCount);
}

} // namespace he::asset
