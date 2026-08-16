#pragma once

#include "RHI/Bindless.h"
#include <vector>

namespace he::rhi {

class VulkanDevice;

// ============================================================
// VulkanBindlessHeap — Vulkan 后端 bindless 描述符堆
//
// 内部维护三种 CPU 侧指针数组（纹理/采样器/SSBO）+ 已登记描述符集。
// Register* 只 push 指针并标 pending；Flush() 用数组版
// UpdateDescriptorSet 写全部已登记 set。
// ============================================================
class VulkanBindlessHeap final : public IRHIBindlessHeap {
public:
    explicit VulkanBindlessHeap(VulkanDevice* device);

    void RegisterDescriptorSet(DescriptorSetHandle set, u32 textureBinding,
                               u32 samplerBinding, u32 bufferBinding = 0) override;
    BindlessHandle RegisterTexture(IRHITexture* texture, IRHISampler* sampler) override;
    BindlessHandle RegisterSampler(IRHISampler* sampler) override;
    BindlessHandle RegisterBuffer(IRHIBuffer* ssbo) override;
    void Flush() override;
    u32 GetTextureCount() const override { return (u32)m_Textures.size(); }
    u32 GetBufferCount() const override { return (u32)m_Buffers.size(); }
    void SetDefaultTexture(IRHITexture* texture, IRHISampler* sampler) override;

private:
    struct RegisteredSet {
        DescriptorSetHandle set;
        u32 textureBinding;
        u32 samplerBinding;
        u32 bufferBinding;
    };

    VulkanDevice* m_Device = nullptr;
    std::vector<RegisteredSet> m_Sets;
    std::vector<IRHITexture*> m_Textures;
    std::vector<IRHISampler*> m_Samplers;
    std::vector<IRHIBuffer*> m_Buffers;
    IRHITexture* m_DefaultTexture = nullptr;
    IRHISampler* m_DefaultSampler = nullptr;
    bool m_Pending = false;
};

} // namespace he::rhi
