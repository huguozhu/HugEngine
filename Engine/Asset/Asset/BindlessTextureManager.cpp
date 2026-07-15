#include "Asset/BindlessTextureManager.h"
#include "Core/Log.h"

// ============================================================
// BindlessTextureManager 实现
//
// 纹理注册 → 标记 m_PendingFlush → FlushPending() 自动向全部
// 已注册描述符集推送。调用方无需关心描述符集数量。
// ============================================================

namespace he::asset {

BindlessTextureManager& BindlessTextureManager::Instance() {
    static BindlessTextureManager mgr;
    return mgr;
}

// --- 描述符集注册 ---

void BindlessTextureManager::RegisterDescriptorSet(
    rhi::IRHIDevice* device, rhi::DescriptorSetHandle set,
    u32 textureBinding, u32 samplerBinding)
{
    m_RegisteredSets.push_back({ device, set, textureBinding, samplerBinding });
    HE_CORE_INFO("BindlessTextureManager: 注册描述符集 handle={:#x} bindings=({},{})",
        (u64)set, textureBinding, samplerBinding);
}

// --- 纹理/材质注册 ---

u32 BindlessTextureManager::RegisterTexture(rhi::IRHITexture* texture, rhi::IRHISampler* sampler) {
    // 空纹理/采样器用占位符替代（避免 Vulkan descriptor write 写入空句柄导致验证层报错）
    if (!texture) texture = m_DefaultTexture;
    if (!sampler) sampler = m_DefaultSampler;
    m_Textures.push_back(texture);
    m_Samplers.push_back(sampler);
    u32 idx = m_TextureCount++;
    m_PendingFlush = true;
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

// --- 同步 ---

void BindlessTextureManager::FlushPending() {
    if (!m_PendingFlush || m_TextureCount == 0) return;

    // 向全部已注册描述符集写入完整纹理/采样器数组
    std::vector<rhi::IRHITexture*> texPtrs(m_Textures.begin(), m_Textures.begin() + m_TextureCount);
    std::vector<rhi::IRHISampler*> sampPtrs(m_Samplers.begin(), m_Samplers.begin() + m_TextureCount);

    for (auto& rs : m_RegisteredSets) {
        rs.device->UpdateDescriptorSet(rs.set, rs.textureBinding,
            rhi::DescriptorType::SampledImage,
            texPtrs.data(), nullptr, m_TextureCount);
        rs.device->UpdateDescriptorSet(rs.set, rs.samplerBinding,
            rhi::DescriptorType::Sampler,
            nullptr, sampPtrs.data(), m_TextureCount);
    }

    m_PendingFlush = false;
    HE_CORE_INFO("BindlessTextureManager: {} textures flushed to {} descriptor sets",
        m_TextureCount, m_RegisteredSets.size());
}

} // namespace he::asset
