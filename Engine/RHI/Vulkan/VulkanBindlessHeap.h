#pragma once

#include "RHI/Bindless.h"
#include "RHI/BindlessSlotRing.h"
#include <vector>

namespace he::rhi {

class VulkanDevice;

// ============================================================
// VulkanBindlessHeap — Vulkan 后端 bindless 描述符堆
//
// 内部维护三种 CPU 侧指针数组（纹理/采样器/SSBO）+ 已登记描述符集。
// Register* 只写指针并标 pending；Flush() 用数组版
// UpdateDescriptorSet 写全部已登记 set。
//
// 任务 23：纹理/缓冲槽位由 BindlessSlotRing 管理 —— Release* 登记回收，BeginFrame 推进，
// 复用槽位时**覆盖**数组元素（数组长度不再随高频更新增长）。
// ============================================================
class VulkanBindlessHeap final : public IRHIBindlessHeap {
public:
    explicit VulkanBindlessHeap(VulkanDevice* device);

    void RegisterDescriptorSet(DescriptorSetHandle set, u32 textureBinding,
                               u32 samplerBinding, u32 bufferBinding = 0) override;
    BindlessHandle RegisterTexture(IRHITexture* texture, IRHISampler* sampler) override;
    BindlessHandle RegisterSampler(IRHISampler* sampler) override;
    BindlessHandle RegisterBuffer(IRHIBuffer* ssbo) override;
    void ReleaseTexture(BindlessHandle handle) override;
    void ReleaseSampler(BindlessHandle handle) override;
    void ReleaseBuffer(BindlessHandle handle) override;
    void BeginFrame(u64 frameIndex) override;
    void Flush() override;
    u32 GetTextureCount() const override { return (u32)m_Textures.size(); }
    u32 GetBufferCount() const override { return (u32)m_Buffers.size(); }
    u32 GetTextureSlotCount() const override { return m_TexRing.GetSlotCount(); }
    u32 GetFreeTextureSlotCount() const override { return m_TexRing.GetFreeSlotCount(); }
    u32 GetBufferSlotCount() const override { return m_BufRing.GetSlotCount(); }
    u32 GetFreeBufferSlotCount() const override { return m_BufRing.GetFreeSlotCount(); }
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
    std::vector<IRHITexture*> m_Textures;   // 下标 = 纹理槽位（与 m_Samplers 同步写）
    std::vector<IRHISampler*> m_Samplers;
    std::vector<IRHIBuffer*>  m_Buffers;
    BindlessSlotRing m_TexRing;             // 纹理槽位环形分配（纹理/采样器成对复用）
    BindlessSlotRing m_BufRing;             // SSBO 槽位环形分配
    u64 m_FrameIndex = 0;                   // 最近一次 BeginFrame 的设备帧号（调试用）
    IRHITexture* m_DefaultTexture = nullptr;
    IRHISampler* m_DefaultSampler = nullptr;
    bool m_Pending = false;
};

} // namespace he::rhi
