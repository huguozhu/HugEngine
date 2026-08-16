#include "VulkanBindlessHeap.h"
#include "VulkanDevice.h"
#include "Core/Log.h"

namespace he::rhi {

VulkanBindlessHeap::VulkanBindlessHeap(VulkanDevice* device) : m_Device(device) {}

void VulkanBindlessHeap::SetDefaultTexture(IRHITexture* texture, IRHISampler* sampler) {
    m_DefaultTexture = texture;
    m_DefaultSampler = sampler;
}

void VulkanBindlessHeap::RegisterDescriptorSet(DescriptorSetHandle set,
                                               u32 textureBinding,
                                               u32 samplerBinding,
                                               u32 bufferBinding) {
    m_Sets.push_back({ set, textureBinding, samplerBinding, bufferBinding });
    HE_CORE_INFO("VulkanBindlessHeap: 注册描述符集 handle={:#x} bindings=({},{},{})",
        (u64)set, textureBinding, samplerBinding, bufferBinding);
}

BindlessHandle VulkanBindlessHeap::RegisterTexture(IRHITexture* texture, IRHISampler* sampler) {
    // 空纹理/采样器用占位符替代（避免 descriptor write 写入空句柄触发验证层报错）
    if (!texture) texture = m_DefaultTexture;
    if (!sampler) sampler = m_DefaultSampler;
    m_Textures.push_back(texture);
    m_Samplers.push_back(sampler);
    m_Pending = true;
    return (BindlessHandle)(m_Textures.size() - 1);
}

BindlessHandle VulkanBindlessHeap::RegisterSampler(IRHISampler* sampler) {
    if (!sampler) sampler = m_DefaultSampler;
    m_Samplers.push_back(sampler);
    m_Pending = true;
    return (BindlessHandle)(m_Samplers.size() - 1);
}

BindlessHandle VulkanBindlessHeap::RegisterBuffer(IRHIBuffer* ssbo) {
    m_Buffers.push_back(ssbo);
    m_Pending = true;
    return (BindlessHandle)(m_Buffers.size() - 1);
}

void VulkanBindlessHeap::Flush() {
    if (!m_Pending) return;
    // 纹理/采样器数组始终同长；向全部已登记 set 写入完整数组
    const u32 texCount = (u32)m_Textures.size();
    const u32 bufCount = (u32)m_Buffers.size();
    for (auto& rs : m_Sets) {
        if (texCount > 0) {
            m_Device->UpdateDescriptorSet(rs.set, rs.textureBinding,
                DescriptorType::SampledImage, m_Textures.data(), nullptr, texCount);
            m_Device->UpdateDescriptorSet(rs.set, rs.samplerBinding,
                DescriptorType::Sampler, nullptr, m_Samplers.data(), texCount);
        }
        if (rs.bufferBinding != 0 && bufCount > 0) {
            m_Device->UpdateDescriptorSet(rs.set, rs.bufferBinding,
                DescriptorType::StorageBuffer, m_Buffers.data(), bufCount);
        }
    }
    m_Pending = false;
    HE_CORE_INFO("VulkanBindlessHeap: flushed {} textures / {} buffers to {} sets",
        texCount, bufCount, m_Sets.size());
}

} // namespace he::rhi
