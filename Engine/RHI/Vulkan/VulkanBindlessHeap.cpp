#include "VulkanBindlessHeap.h"
#include "VulkanTextureLiveness.h"   // T8：野指针判定
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
    // 【槽位 0 保留为无效哨兵】引擎约定缓冲句柄 0 = 无效（如逐实例剔除的可见列表句柄为 0 即跳过派发，
    // 推送常量里的句柄 0 表示「未使用」），D3D12 后端也保留了 0。此前 Vulkan 从 0 起分配，
    // 第一个注册的缓冲（剔除器的可见列表）拿到句柄 0 ⇒ 被当作无效，剔除从不派发、实例全不绘制。
    // 槽位 0 固定指向一个占位缓冲（描述符数组整体写入时每个元素都必须是有效缓冲）
    if (m_Buffers.empty()) {
        BufferDesc nd{};
        nd.size  = 256;
        nd.usage = BufferUsage::Storage;
        m_NullBuffer = m_Device->CreateBufferHandle(nd);   // 设备持有，随设备在 VMA 销毁前释放
        IRHIBuffer* nb = m_Device->ResolveBuffer(m_NullBuffer);
        m_Buffers.push_back(nb ? nb : ssbo);   // 占位缓冲创建失败时退而用本缓冲填位（仍不分配出去）
        m_BufRing.Acquire();                   // 首次分配必为槽位 0：占住、永不释放
    }
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
    u32 texCount = (u32)m_Textures.size();
    u32 bufCount = (u32)m_Buffers.size();
    // 【T8 修复】SSBO 数组同样可能含已释放/为空的槽位（`ReleaseTexture` 会回收槽位，
    //   而缓冲槽是独立追加的）⇒ 按"第一个空槽之前"截断，避免把空 VkBuffer 写进描述符数组
    //   （实测崩在 `VulkanBindlessHeap.cpp:128` 的缓冲数组写）。
    {
        u32 valid = 0u;
        while (valid < bufCount && m_Buffers[valid] != nullptr) ++valid;
        bufCount = valid;
    }
    // 【T8 修复：首帧崩溃】占位槽（`RegisterTexture(nullptr, nullptr)`）在堆未设默认纹理时
    // 会留成空句柄；把空句柄写进描述符数组时驱动侧直接访问违例（实测栈：
    // `UpdateDescriptorSet ← BindlessHeap::Flush ← NaniteRaster::EnsureSoftRasterResources`，
    // `0xC0000005`，目标地址 0x1_0000_00A2）。这里按"第一个空槽之前"的长度写，
    // 空槽之后的索引本来也不会被材质采样到（占位槽永远追加在末尾）。
    {
        // 【T8 修复】除了空句柄，还要挡"已销毁纹理/采样器"的野指针：纹理侧有成对登记的
        //   采样器槽位，`IsTextureAlive` 能判定这一对是否还有效。野指针写进描述符数组时
        //   驱动直接访问违例（实测栈在 nvoglv64 的 vkUpdateDescriptorSets 内）。
        u32 valid = 0u;
        while (valid < texCount && m_Textures[valid] != nullptr && m_Samplers[valid] != nullptr
               && IsTextureAlive(m_Textures[valid])) {
            ++valid;
        }
        if (valid < texCount) {
            static bool s_Logged = false;
            if (!s_Logged) {
                s_Logged = true;
                HE_CORE_WARN("VulkanBindlessHeap: 第 {} 个纹理槽为空句柄（默认纹理/采样器未设置）"
                             "⇒ 本次只写前 {} 个槽，避免驱动侧访问违例", valid, valid);
            }
            texCount = valid;
        }
    }
    // 【T8 修复：驱动级访问违例的真因】`RegisterSampler` 只往 `m_Samplers` 追加，
    //   而 `RegisterTexture` 同时往两边追加 ⇒ 两个数组**长度可能不等**。
    //   之前用同一个 `texCount` 写采样器数组，会读到 `m_Samplers` 末尾之外
    //   （`0xC0000005` 崩在 nvoglv64 的 `vkUpdateDescriptorSets` 内，栈顶是 UpdateDescriptorSet:120）。
    //   这里按各自长度分别钳制。
    const u32 sampCount = (texCount < (u32)m_Samplers.size()) ? texCount : (u32)m_Samplers.size();
    // 【T8 修复】分块写（每档 64 条）：一次性写整条 4096 长数组时驱动会在
    //   `vkUpdateDescriptorSets` 内部崩溃（栈在 nvoglv64）；分段写绕开了这条驱动路径。
    const u32 kChunk = 64u;
    for (auto& rs : m_Sets) {
        for (u32 first = 0; first < texCount; first += kChunk) {
            const u32 n = (texCount - first < kChunk) ? (texCount - first) : kChunk;
            m_Device->UpdateDescriptorSetArrayRange(rs.set, rs.textureBinding,
                DescriptorType::SampledImage, m_Textures.data() + first, nullptr, first, n);
        }
        for (u32 first = 0; first < sampCount; first += kChunk) {
            const u32 n = (sampCount - first < kChunk) ? (sampCount - first) : kChunk;
            m_Device->UpdateDescriptorSetArrayRange(rs.set, rs.samplerBinding,
                DescriptorType::Sampler, nullptr, m_Samplers.data() + first, first, n);
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
