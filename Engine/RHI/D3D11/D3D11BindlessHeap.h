// ============================================================================
//  D3D11BindlessHeap.h —— D3D11 下的「bindless 堆」最小实现
//
//  D3D11 / SM 5.0 没有 bindless：着色器不能用运行时句柄索引资源数组
//  （替代方案见 D3D11 后端实现规划 3.2：逐 draw 绑定 / 纹理数组桶）。
//  但引擎会**无条件**取 GetBindlessHeap() 并注册资源（D3D12 后端“静默挂起”教训 #4：
//  返回空指针会让引擎解引用空指针），所以这里提供一个只登记、不上 GPU 的实现：
//    · Register* 返回递增句柄并记下资源，供 M3 的逐 draw 绑定按句柄查回资源；
//    · Flush / 回收都是空操作（没有 GPU 侧描述符要写）。
// ============================================================================
#pragma once

#if defined(_WIN32)

#include "RHI/Bindless.h"
#include <vector>

namespace he::rhi::d3d11 {

class D3D11BindlessHeap final : public IRHIBindlessHeap {
public:
    void RegisterDescriptorSet(DescriptorSetHandle, u32, u32, u32 = 0) override {}

    BindlessHandle RegisterTexture(IRHITexture* texture, IRHISampler* sampler) override {
        m_Textures.push_back({ texture ? texture : m_DefaultTexture, sampler ? sampler : m_DefaultSampler });
        return static_cast<BindlessHandle>(m_Textures.size() - 1);
    }
    BindlessHandle RegisterSampler(IRHISampler* sampler) override {
        m_Samplers.push_back(sampler ? sampler : m_DefaultSampler);
        return static_cast<BindlessHandle>(m_Samplers.size() - 1);
    }
    BindlessHandle RegisterBuffer(IRHIBuffer* ssbo) override {
        m_Buffers.push_back(ssbo);
        return static_cast<BindlessHandle>(m_Buffers.size() - 1);
    }

    void Flush() override {}

    u32 GetTextureCount() const override { return static_cast<u32>(m_Textures.size()); }
    u32 GetBufferCount()  const override { return static_cast<u32>(m_Buffers.size()); }

    void SetDefaultTexture(IRHITexture* texture, IRHISampler* sampler) override {
        m_DefaultTexture = texture;
        m_DefaultSampler = sampler;
    }

    // ---- 供 D3D11 后端的逐 draw 绑定按句柄查回资源（越界返回空）----
    IRHITexture* GetTexture(BindlessHandle h) const { return h < m_Textures.size() ? m_Textures[h].texture : nullptr; }
    IRHISampler* GetTextureSampler(BindlessHandle h) const { return h < m_Textures.size() ? m_Textures[h].sampler : nullptr; }
    IRHIBuffer*  GetBuffer(BindlessHandle h) const { return h < m_Buffers.size() ? m_Buffers[h] : nullptr; }

private:
    struct TextureEntry { IRHITexture* texture = nullptr; IRHISampler* sampler = nullptr; };
    std::vector<TextureEntry> m_Textures;
    std::vector<IRHISampler*> m_Samplers;
    std::vector<IRHIBuffer*>  m_Buffers;
    IRHITexture* m_DefaultTexture = nullptr;
    IRHISampler* m_DefaultSampler = nullptr;
};

} // namespace he::rhi::d3d11

#endif // _WIN32
