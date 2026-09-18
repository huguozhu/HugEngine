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
    // 任务 23：优先复用已回收的槽位（覆盖数组元素），否则追加新槽
    const u32 slot = m_TexRing.Acquire();
    if (slot < (u32)m_Textures.size()) {
        m_Textures[slot] = texture;      // 复用：数组长度不变（原来 append-only 会无限增长）
        m_Samplers[slot] = sampler;
    } else {
        m_Textures.push_back(texture);
        m_Samplers.push_back(sampler);
    }
    m_Pending = true;
    return (BindlessHandle)slot;
}

BindlessHandle VulkanBindlessHeap::RegisterSampler(IRHISampler* sampler) {
    if (!sampler) sampler = m_DefaultSampler;
    // 独立采样器（不与纹理配对）仍走追加：它的下标空间是"采样器数组"，与纹理槽位语义不同，
    // 混用环形回收会让两套下标互相踩踏。引擎内目前无调用点（纹理注册已含采样器）。
    m_Samplers.push_back(sampler);
    m_Pending = true;
    return (BindlessHandle)(m_Samplers.size() - 1);
}

BindlessHandle VulkanBindlessHeap::RegisterBuffer(IRHIBuffer* ssbo) {
    const u32 slot = m_BufRing.Acquire();
    if (slot < (u32)m_Buffers.size()) m_Buffers[slot] = ssbo;   // 复用槽位
    else                              m_Buffers.push_back(ssbo);
    m_Pending = true;
    return (BindlessHandle)slot;
}

void VulkanBindlessHeap::ReleaseTexture(BindlessHandle handle) {
    m_TexRing.Release(handle);
}

void VulkanBindlessHeap::ReleaseSampler(BindlessHandle handle) {
    // 纹理槽位与采样器槽位一一对应（RegisterTexture 成对写入），因此释放纹理槽位即释放采样器槽位；
    // 独立注册的采样器走追加、不支持回收（见 RegisterSampler）。
    m_TexRing.Release(handle);
}

void VulkanBindlessHeap::ReleaseBuffer(BindlessHandle handle) {
    m_BufRing.Release(handle);
}

void VulkanBindlessHeap::BeginFrame(u64 frameIndex) {
    m_FrameIndex = frameIndex;
    // 保护期（= 飞行帧数）已过的槽位回到空闲表，供后续 Register* 复用
    m_TexRing.BeginFrame();
    m_BufRing.BeginFrame();
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
