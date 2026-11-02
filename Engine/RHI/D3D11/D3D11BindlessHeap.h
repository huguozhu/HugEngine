// ============================================================================
//  D3D11BindlessHeap.h —— D3D11 下的「bindless 堆」
//
//  D3D11 / SM 5.0 没有 bindless：着色器不能用运行时句柄索引资源数组
//  （替代方案见 D3D11 后端实现规划 3.2：逐 draw 绑定 / 纹理数组桶）。
//  但引擎会**无条件**取 GetBindlessHeap() 并注册资源（D3D12 后端“静默挂起”教训 #4：
//  返回空指针会让引擎解引用空指针），所以这里提供一个只登记、不上 GPU 的实现：
//    · Register* 返回递增句柄并记下资源，供逐绘制绑定（SetNonBindlessHandle）按句柄查回；
//    · 回收是空操作（没有 GPU 侧描述符要写）。
//
//  【登记时就持有 D3D11 视图】Vulkan 的描述符在写入时就拷走了视图，引擎因此会在注册之后
//  销毁纹理 / 采样器包装对象（如 GBufferRenderer 的占位纹理是局部变量）。这里同样在注册时
//  取出 SRV / 采样器状态并持有 COM 引用，包装对象销毁后底层 D3D11 对象仍然有效
//  （只存裸指针会在之后绑定时传入悬空指针，实测调试层里访问违例）。
//
//  【纹理数组桶（M6）】一次绘制覆盖多个材质的 GPU 驱动路径拿不到逐绘制句柄：这里把可分桶的
//  静态纹理按（宽, 高, mip 数, 格式）拷进 ≤ 8 个 Texture2DArray，并维护「句柄 → (桶号, 层号)」
//  映射表，着色器按句柄查表后对字面量下标的桶采样（见 common.slang 的 HE_SHADER_D3D11 分支）。
//  拷贝在 Flush / 首次绑定前集中进行（EnsureBuckets）。
// ============================================================================
#pragma once

#if defined(_WIN32)

#include "RHI/Bindless.h"
#include "D3D11Resources.h"
#include <vector>

namespace he::rhi::d3d11 {

class D3D11Device;

class D3D11BindlessHeap final : public IRHIBindlessHeap {
public:
    /// 设备创建后调用：分桶要用设备建纹理数组、用 immediate context 拷贝
    void Attach(D3D11Device* device) { m_Device = device; }

    void RegisterDescriptorSet(DescriptorSetHandle, u32, u32, u32 = 0) override {}

    // 空纹理 / 空采样器记为空，取用时换成占位资源：引擎可能先注册、后 SetDefaultTexture
    BindlessHandle RegisterTexture(IRHITexture* texture, IRHISampler* sampler) override;
    BindlessHandle RegisterSampler(IRHISampler* sampler) override {
        m_Samplers.push_back(StateOf(sampler));
        return static_cast<BindlessHandle>(m_Samplers.size() - 1);
    }
    BindlessHandle RegisterBuffer(IRHIBuffer* ssbo) override {
        m_Buffers.push_back(ssbo);
        return static_cast<BindlessHandle>(m_Buffers.size() - 1);
    }

    /// 引擎每帧调用：把新登记的纹理拷进桶
    void Flush() override { EnsureBuckets(); }

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

    // ---- 纹理数组桶 ----
    /// 把待处理的纹理拷进桶、按需扩容并更新映射表（只用 immediate context，在录制线程上调用）
    void EnsureBuckets();
    /// 第 k 个桶的 SRV（Texture2DArray，全部 mip / 层）；还没建的桶返回空（映射表不会指向它）
    ID3D11ShaderResourceView* GetBucketSrv(u32 k) const;
    /// 句柄 → (桶号 << 24 | 层号) 的映射表（StructuredBuffer<uint>）
    ID3D11ShaderResourceView* GetBucketMapSrv() const { return m_MapSrv.Get(); }
    /// 桶路径的采样器（线性、重复寻址）
    ID3D11SamplerState*       GetBucketSampler() const { return m_BucketSampler.Get(); }

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
        bool                             bucketable = false;   // 静态 2D 纹理，可以拷进桶
    };

    /// 一个纹理数组桶：同尺寸、同 mip 数、同格式的纹理各占一层
    struct Bucket {
        u32 width = 0, height = 0, mips = 0;
        DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN;   // 纹理数组的资源格式（与源纹理相同，才能直接拷贝）
        DXGI_FORMAT srvFormat      = DXGI_FORMAT_UNKNOWN;   // 采样用的视图格式（如 sRGB）
        u32 capacity = 0, used = 0;                         // 已分配 / 已使用的层数
        ComPtr<ID3D11Texture2D>          array;
        ComPtr<ID3D11ShaderResourceView> srv;
    };
    /// 找到（或新建）与该纹理匹配的桶；桶已满 8 个时返回 -1
    int  FindOrCreateBucket(u32 width, u32 height, u32 mips, DXGI_FORMAT resourceFormat, DXGI_FORMAT srvFormat);
    /// 保证桶至少能放 layers 层（翻倍扩容，旧内容逐子资源拷过去）；失败返回 false
    bool GrowBucket(Bucket& b, u32 layers);
    /// 映射表容量不够时重建，并把 CPU 副本整体上传
    void UploadMap();

    D3D11Device*                            m_Device = nullptr;
    std::vector<TextureEntry>               m_Textures;
    std::vector<ComPtr<ID3D11SamplerState>> m_Samplers;
    std::vector<IRHIBuffer*>                m_Buffers;
    ComPtr<ID3D11ShaderResourceView>        m_DefaultSrv;
    ComPtr<ID3D11SamplerState>              m_DefaultSampler;

    std::vector<Bucket>                     m_Buckets;          // 最多 kBindingNBTexBucketCount 个
    std::vector<u32>                        m_Map;              // 句柄 → (桶号 << 24 | 层号)，CPU 副本
    std::vector<BindlessHandle>             m_PendingBucket;    // 已登记、还没拷进桶的句柄
    bool                                    m_MapDirty = false;
    u32                                     m_MapCapacity = 0;  // GPU 映射表的元素数
    ComPtr<ID3D11Buffer>                    m_MapBuffer;
    ComPtr<ID3D11ShaderResourceView>        m_MapSrv;
    ComPtr<ID3D11SamplerState>              m_BucketSampler;
    bool                                    m_WarnedFull = false;
};

} // namespace he::rhi::d3d11

#endif // _WIN32
