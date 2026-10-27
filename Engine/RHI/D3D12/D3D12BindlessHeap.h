// ============================================================================
//  D3D12BindlessHeap.h —— D3D12 的 bindless 堆
//
//  【为什么需要它】引擎的 ForwardPipeline 会**无条件**解引用
//  `device->GetBindlessHeap()`（ForwardPipeline.cpp:200-207），而 D3D12 后端
//  早期版本返回 nullptr ⇒ 空指针调用 ⇒ 进程静默挂起（实测 02.Cube 卡死在此）。
//
//  【当前实现范围】这是一个**功能可用但简化**的版本：
//    · 纹理/采样器/缓冲各自分配递增的槽位号
//    · 真正的 GPU 侧绑定留给着色器侧（D3D12 用描述符堆 + 槽位号）
//    · 已注册的资源会写进设备级的 shader-visible 描述符堆
//  完整的 bindless（64K 槽、空闲链表、帧回收）在阶段 2 补齐。
// ============================================================================
#pragma once

#if defined(_WIN32)

#include "D3D12Common.h"
#include <vector>

namespace he::rhi::d3d12 {

class D3D12Device;

class D3D12BindlessHeap final : public IRHIBindlessHeap {
public:
    explicit D3D12BindlessHeap(D3D12Device* device) : m_Device(device) {}

    // 引擎通过它告知 bindless 的纹理/采样器/缓冲分别绑在哪个 binding 上；
    // D3D12 侧用固定槽位 + 描述符堆表达，这里只需记下（阶段 2 用它建表）。
    void RegisterDescriptorSet(DescriptorSetHandle set, u32 textureBinding,
                               u32 samplerBinding, u32 bufferBinding = 0) override {
        (void)set; (void)textureBinding; (void)samplerBinding; (void)bufferBinding;
    }

    BindlessHandle RegisterTexture(IRHITexture* texture, IRHISampler* sampler) override;
    BindlessHandle RegisterSampler(IRHISampler* sampler) override;
    BindlessHandle RegisterBuffer(IRHIBuffer* ssbo) override;

    void Flush() override {}
    u32  GetTextureCount() const override { return static_cast<u32>(m_Textures.size()); }
    u32  GetBufferCount()  const override { return static_cast<u32>(m_Buffers.size()); }

    void SetDefaultTexture(IRHITexture* texture, IRHISampler* sampler) override;

private:
    D3D12Device* m_Device = nullptr;
    struct Entry { IRHITexture* tex = nullptr; IRHISampler* smp = nullptr; };
    std::vector<Entry> m_Textures;   // 槽位 0 保留为默认纹理
    std::vector<IRHISampler*> m_Samplers;
    std::vector<IRHIBuffer*>  m_Buffers;
};

} // namespace he::rhi::d3d12

#endif // _WIN32
