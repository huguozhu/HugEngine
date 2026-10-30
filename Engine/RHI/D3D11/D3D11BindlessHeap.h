// ============================================================================
//  D3D11BindlessHeap.h —— D3D11 下的「bindless 堆」最小实现
//
//  D3D11 / SM 5.0 没有 bindless：着色器不能用运行时句柄索引资源数组
//  （替代方案见 D3D11 后端实现规划 3.2：逐 draw 绑定 / 纹理数组桶）。
//  但引擎会**无条件**取 GetBindlessHeap() 并注册资源（D3D12 后端“静默挂起”教训 #4：
//  返回空指针会让引擎解引用空指针），所以这里提供一个只登记、不上 GPU 的实现：
//    · Register* 返回递增句柄并记下资源，供逐绘制绑定（SetNonBindlessHandle）按句柄查回；
//    · Flush / 回收都是空操作（没有 GPU 侧描述符要写）。
//
//  【登记时就持有 D3D11 视图】Vulkan 的描述符在写入时就拷走了视图，引擎因此会在注册之后
//  销毁纹理 / 采样器包装对象（如 GBufferRenderer 的占位纹理是局部变量）。这里同样在注册时
//  取出 SRV / 采样器状态并持有 COM 引用，包装对象销毁后底层 D3D11 对象仍然有效
//  （只存裸指针会在之后绑定时传入悬空指针，实测调试层里访问违例）。
// ============================================================================
#pragma once

#if defined(_WIN32)

#include "RHI/Bindless.h"
#include "D3D11Resources.h"
#include <vector>

namespace he::rhi::d3d11 {

class D3D11BindlessHeap final : public IRHIBindlessHeap {
public:
    void RegisterDescriptorSet(DescriptorSetHandle, u32, u32, u32 = 0) override {}

    // 空纹理 / 空采样器记为空，取用时换成占位资源：引擎可能先注册、后 SetDefaultTexture
    BindlessHandle RegisterTexture(IRHITexture* texture, IRHISampler* sampler) override {
        m_Textures.push_back({ SrvOf(texture), StateOf(sampler) });
        return static_cast<BindlessHandle>(m_Textures.size() - 1);
    }
    BindlessHandle RegisterSampler(IRHISampler* sampler) override {
        m_Samplers.push_back(StateOf(sampler));
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
        m_DefaultSrv = SrvOf(texture);
        m_DefaultSampler = StateOf(sampler);
    }

    // ---- 供 D3D11 后端的逐绘制绑定按句柄查回资源（空槽 / 越界返回占位资源，与 Vulkan 的空槽回退一致）----
    ID3D11ShaderResourceView* GetTextureSrv(BindlessHandle h) const {
        ID3D11ShaderResourceView* v = h < m_Textures.size() ? m_Textures[h].srv.Get() : nullptr;
        return v ? v : m_DefaultSrv.Get();
    }
    ID3D11SamplerState* GetTextureSampler(BindlessHandle h) const {
        ID3D11SamplerState* s = h < m_Textures.size() ? m_Textures[h].sampler.Get() : nullptr;
        return s ? s : m_DefaultSampler.Get();
    }
    /// 缓冲按裸指针记录（视图要等绑定时按着色器反射的步长才能建）
    IRHIBuffer* GetBuffer(BindlessHandle h) const { return h < m_Buffers.size() ? m_Buffers[h] : nullptr; }

private:
    static ComPtr<ID3D11ShaderResourceView> SrvOf(IRHITexture* t) {
        return t ? ComPtr<ID3D11ShaderResourceView>(static_cast<D3D11Texture*>(t)->GetSrv()) : nullptr;
    }
    static ComPtr<ID3D11SamplerState> StateOf(IRHISampler* s) {
        return s ? ComPtr<ID3D11SamplerState>(static_cast<D3D11Sampler*>(s)->GetState()) : nullptr;
    }

    struct TextureEntry {
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11SamplerState>       sampler;
    };
    std::vector<TextureEntry>               m_Textures;
    std::vector<ComPtr<ID3D11SamplerState>> m_Samplers;
    std::vector<IRHIBuffer*>                m_Buffers;
    ComPtr<ID3D11ShaderResourceView>        m_DefaultSrv;
    ComPtr<ID3D11SamplerState>              m_DefaultSampler;
};

} // namespace he::rhi::d3d11

#endif // _WIN32
