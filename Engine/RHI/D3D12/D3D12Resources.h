// ============================================================================
//  D3D12Resources.h —— D3D12 的资源与管线对象
//
//  包含 Buffer / Texture / Sampler / PipelineState 四个实现。
//  设计要点：
//    · Buffer：DEFAULT 堆用于 GPU 资源，UPLOAD 堆用于 CPU 频繁写入，
//      READBACK 堆用于回读。cpuAccess 决定堆类型。
//    · Texture：DEFAULT 堆 + RTV/DSV/SRV 描述符（描述符从设备的分堆里分配）。
//    · Sampler：D3D12 的采样器也是描述符，放在独立的 SAMPLER 堆里。
//    · PipelineState：由根签名 + PSO 组成；阶段 2 完成完整实现，
//      当前只建立 PSO（根签名先用空根签名占位）。
// ============================================================================
#pragma once

#if defined(_WIN32)

#include "D3D12Common.h"
#include "RHI/Shader.h"
#include "RHI/QueryPool.h"
#include <vector>

namespace he::rhi::d3d12 {

class D3D12Device;

// ---------------------------------------------------------------------------
//  Buffer
// ---------------------------------------------------------------------------
class D3D12Buffer final : public IRHIBuffer {
public:
    ~D3D12Buffer() override;

    bool Initialize(D3D12Device* device, const BufferDesc& desc);

    usize  GetSize() const override { return m_Size; }
    void*  Map() override;
    void   Unmap() override;
    u64    GetDeviceAddress() const override { return m_GpuAddress; }

    /// 供 CommandList 使用：顶点/索引/常量/间接参数视图
    ID3D12Resource* GetResource() const { return m_Resource.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS GetGpuAddress() const { return m_GpuAddress; }
    D3D12_VERTEX_BUFFER_VIEW   GetVbv(u32 stride) const;
    D3D12_INDEX_BUFFER_VIEW    GetIbv() const;

    /// 创建时记录的顶点步长（0 表示不是顶点缓冲）
    u32 GetStride() const { return m_Stride; }

    /// 供 CommandList 使用：拿到底层资源
    ID3D12Resource* GetNativeHandle() const { return m_Resource.Get(); }

private:
    ComPtr<ID3D12Resource> m_Resource;
    D3D12_GPU_VIRTUAL_ADDRESS m_GpuAddress = 0;
    usize m_Size = 0;
    u32   m_Stride = 0;
    /// 资源的当前 D3D12 状态（屏障要求 StateBefore 与实际一致）
    D3D12_RESOURCE_STATES m_State = D3D12_RESOURCE_STATE_COMMON;
    bool  m_PersistentlyMapped = false;
    void* m_MappedPtr = nullptr;
    ComPtr<ID3D12Resource> m_UploadStaging;   // initialData 的上传中转
    // UAV 缓冲的 CPU 暂存：D3D12 禁止在 UPLOAD 堆上建 UAV 缓冲，
    // 所以「CPU 可写 + GPU 可写」的缓冲需要 DEFAULT 主资源 + UPLOAD 暂存，
    // Unmap 时用 CopyBufferRegion 把暂存拷到主资源。
    ComPtr<ID3D12Resource> m_CpuStaging;
    bool                   m_NeedsStagingCopy = false;
    D3D12Device*           m_Device = nullptr;
};

// ---------------------------------------------------------------------------
//  Texture
// ---------------------------------------------------------------------------
class D3D12Texture final : public IRHITexture {
public:
    ~D3D12Texture() override;

    /// transient = true：资源放在设备的逐帧瞬态堆上（placed resource），堆空间不足时返回 false
    bool Initialize(D3D12Device* device, const TextureDesc& desc, bool transient = false);

    u32    GetWidth()  const override { return m_Width; }
    u32    GetHeight() const override { return m_Height; }
    u32    GetDepth()  const override { return m_Depth; }
    u32    GetMipLevels()   const override { return m_MipLevels; }
    u32    GetArrayLayers() const override { return m_ArrayLayers; }
    Format GetFormat() const override { return m_Format; }
    void*  GetNativeHandle() const override {
        // 【语义对齐】引擎把 GetNativeHandle() 当「图像视图」用（Vulkan 侧返回
        // VkImageView，直接传给 BeginOffscreenPass 当 RTV/DSV）。之前这里返回
        // ID3D12Resource*，命令列表按 CPU 描述符句柄解释后得到垃圾指针，
        // OMSetRenderTargets 收到非法 DSV 会让驱动卡死（实测 02.Cube 第一帧
        // 的 CSM 阴影 pass 就死在这里）。
        if (m_IsDepth && m_Dsv.ptr) return reinterpret_cast<void*>(m_Dsv.ptr);
        if (m_Rtv.ptr)              return reinterpret_cast<void*>(m_Rtv.ptr);
        if (m_Dsv.ptr)              return reinterpret_cast<void*>(m_Dsv.ptr);
        // 既非渲染目标也非深度（如只做 UAV 的 compute 目标）：返回整资源视图键，
        // 引擎把它交给 UpdateDescriptorSetWithImageView 绑定（Vulkan 侧是整图 ImageView）
        if (m_ViewKey.ptr)          return reinterpret_cast<void*>(m_ViewKey.ptr);
        return nullptr;
    }
    /// 逐面/逐层视图（与 Vulkan 的逐层 ImageView 对应）：渲染目标返回该层的 RTV，
    /// 深度返回该层的 DSV（mip 0），供立方体图逐面渲染（IBL 烘焙、点光源阴影）使用
    void*  GetNativeHandle(u32 index) const override {
        if (index >= m_FaceViews.size()) return nullptr;
        return reinterpret_cast<void*>(m_FaceViews[index].ptr);
    }

    ID3D12Resource* GetResource() const { return m_Resource.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetRtv() const { return m_Rtv; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetDsv() const { return m_Dsv; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetSrv(u32 mip = 0) const {
        return (mip < m_Srvs.size()) ? m_Srvs[mip] : D3D12_CPU_DESCRIPTOR_HANDLE{};
    }
    bool IsDepth() const { return m_IsDepth; }

private:
    /// 上传 TextureDesc::initialData（各数组层的 mip 0，紧密排列，与 Vulkan 后端一致），
    /// 完成后把资源转成着色器可读状态。同步等待 GPU 拷贝完成。
    void UploadInitialData(D3D12Device* device, const TextureDesc& desc, D3D12_RESOURCE_STATES curState);

private:
    ComPtr<ID3D12Resource> m_Resource;
    ComPtr<ID3D12Resource> m_UploadStaging;
    D3D12_CPU_DESCRIPTOR_HANDLE m_Rtv{};
    // 【必须持有堆】RTV/DSV 的描述符堆如果只是局部变量，作用域结束就被释放，
    // m_Rtv/m_Dsv 会指向已释放的堆 ⇒ 变成垃圾描述符 ⇒
    // OMSetRenderTargets 收到非法 RTV 让驱动静默卡死（实测 02.Cube 死在这里）。
    ComPtr<ID3D12DescriptorHeap> m_RtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_DsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_Dsv{};
    D3D12_CPU_DESCRIPTOR_HANDLE m_DsvReadOnly{};   // 只读深度视图（深度测试 + 同时采样）
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_Srvs;
    ComPtr<ID3D12DescriptorHeap>             m_FaceViewHeap;   // 逐层 RTV/DSV 的堆
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_FaceViews;      // 每个数组层一个视图（mip 0）
    /// 无 RTV/DSV 的纹理的整资源视图键（CPU 侧堆里的一个 SRV），见 GetNativeHandle
    ComPtr<ID3D12DescriptorHeap>             m_ViewKeyHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE              m_ViewKey{};
    u32    m_Width = 1, m_Height = 1, m_Depth = 1;
    u32    m_MipLevels = 1, m_ArrayLayers = 1;
    Format m_Format = Format::RGBA8_UNORM;
    bool   m_IsDepth = false;
    D3D12Device* m_OwnerDevice = nullptr;   // 析构时注销视图映射用
};

// ---------------------------------------------------------------------------
//  QueryPool —— GPU 查询（时间戳 / 管线统计）
//
//  · ID3D12QueryHeap 存查询，写完立刻 ResolveQueryData 到持久映射的 READBACK 缓冲
//  · 可用性：命令列表提交时给本次写过的查询打上「围栏 + 值」，围栏到达即可读取
//   （对应 Vulkan 的 VK_QUERY_RESULT_WAIT / WITH_AVAILABILITY 语义）
// ---------------------------------------------------------------------------
class D3D12QueryPool final : public IRHIQueryPool {
public:
    bool Initialize(D3D12Device* device, u32 queryCount, QueryType type);

    u32       GetQueryCount() const override { return m_Count; }
    QueryType GetQueryType()  const override { return m_Type; }

    ID3D12QueryHeap* GetHeap() const { return m_Heap.Get(); }
    ID3D12Resource*  GetReadback() const { return m_Readback.Get(); }
    /// 每个查询在回读缓冲中的字节数
    u32              GetResultStride() const { return m_Stride; }

    /// 提交时调用：查询 index 的结果在 fence 到达 value 后可读
    void MarkSubmitted(u32 index, ID3D12Fence* fence, u64 value);
    /// 重置：全部查询变为「不可用」
    void ResetAll();
    /// 查询是否已可读；wait 为真时最多等待 timeoutMs 毫秒（设备挂起时不至于永久阻塞）
    bool IsAvailable(u32 index, bool wait, u32 timeoutMs = 2000) const;
    /// 原始结果指针（指向已映射的回读缓冲）
    const u8* ResultPtr(u32 index) const {
        return m_Mapped ? m_Mapped + static_cast<size_t>(index) * m_Stride : nullptr;
    }

private:
    ComPtr<ID3D12QueryHeap> m_Heap;
    ComPtr<ID3D12Resource>  m_Readback;
    const u8*               m_Mapped = nullptr;
    u32                     m_Count  = 0;
    u32                     m_Stride = 8;
    QueryType               m_Type   = QueryType::Timestamp;
    /// 每个查询的可用条件；fence 为空表示尚未写入（或已重置）
    struct Avail { ComPtr<ID3D12Fence> fence; u64 value = 0; };
    std::vector<Avail>      m_Avail;
};

// ---------------------------------------------------------------------------
//  Sampler
// ---------------------------------------------------------------------------
class D3D12Sampler final : public IRHISampler {
public:
    ~D3D12Sampler() override;

    bool Initialize(D3D12Device* device, const SamplerDesc& desc);

    D3D12_CPU_DESCRIPTOR_HANDLE GetDescriptor() const { return m_Descriptor; }

private:
    D3D12_CPU_DESCRIPTOR_HANDLE m_Descriptor{};
    ComPtr<ID3D12DescriptorHeap> m_Heap;   // 每个采样器一个 1-slot 堆（阶段 1 简化）
};

// ---------------------------------------------------------------------------
//  PipelineState
// ---------------------------------------------------------------------------
class D3D12PipelineState final : public IRHIPipelineState {
public:
    ~D3D12PipelineState() override;

    bool Initialize(D3D12Device* device, const PipelineStateDesc& desc);

    void* GetNativeHandle() const override { return m_Pso.Get(); }

    ID3D12PipelineState*   GetPso() const { return m_Pso.Get(); }
    ID3D12RootSignature*   GetRootSignature() const { return m_RootSig.Get(); }
    D3D12_PRIMITIVE_TOPOLOGY GetTopology() const { return m_Topology; }
    u32 GetVertexStride() const { return m_VertexStride; }
    /// 是否计算管线（图形/计算根签名是两套独立绑定）
    bool IsCompute() const { return m_IsCompute; }
    /// 是否网格管线（MS / AS + PS，无输入装配）
    bool IsMesh() const { return m_IsMesh; }
    bool HasPixelShader() const { return m_HasPs; }
    void SetHasPixelShader(bool v) { m_HasPs = v; }
    /// 零缓冲所在的输入槽：着色器声明了、但引擎顶点布局未提供的属性都从这里取 0
    static constexpr u32 kZeroVertexSlot = 15;
    /// 是否有属性挂在零缓冲槽上（SetPipeline 时需要绑定零缓冲）
    bool NeedsZeroVertexBuffer() const { return m_NeedsZeroVB; }
    /// 实例序号缓冲所在的输入槽：顶点着色器的实例号经 ShaderToDxil 改为逐实例属性 HE_INSTANCEINDEX，
    /// 从这里绑定的 0,1,2… 序号缓冲读取。IA 取逐实例数据时会加上起始实例（StartInstanceLocation），
    /// 读到的值即「起始实例 + 实例号」，与 Vulkan 的 gl_InstanceIndex 一致（SV_InstanceID 不含起始实例）
    static constexpr u32 kInstanceIndexSlot = 14;
    /// 顶点着色器是否读实例号（SetPipeline 时需要绑定实例序号缓冲）
    bool NeedsInstanceIndexBuffer() const { return m_NeedsInstanceIndexVB; }
    /// 附件加载操作（与 Vulkan 的 render pass loadOp 对应）：离屏 pass 据此决定是否清空附件
    LoadOp GetColorLoadOp() const { return m_ColorLoadOp; }
    LoadOp GetDepthLoadOp() const { return m_DepthLoadOp; }
    /// 是否写深度：不写深度的 PSO 可以配只读 DSV，从而同时采样同一张深度图
    bool   GetDepthWrite()   const { return m_DepthWrite; }

    /// 着色器实际使用的描述符集资源（来自 DXIL 反射）：绘制/派发前据此补资源状态转换
    struct ResourceUse { u32 space = 0; u32 binding = 0; bool uav = false; };
    const std::vector<ResourceUse>& GetResourceUses() const { return m_ResourceUses; }

private:
    /// 网格管线的创建（管线状态流）：gfx 是已填好的图形管线状态，着色器以外的状态从这里取
    bool InitializeMeshPso(D3D12Device* device, const PipelineStateDesc& desc,
                           const D3D12_GRAPHICS_PIPELINE_STATE_DESC& gfx,
                           u64 psoHash, const std::vector<u8>* cachedBlob);

    ComPtr<ID3D12RootSignature> m_RootSig;
    ComPtr<ID3D12PipelineState> m_Pso;
    bool m_IsCompute = false;
    bool m_IsMesh    = false;
    bool m_HasPs = false;
    D3D12_PRIMITIVE_TOPOLOGY    m_Topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    /// 输入布局的总步长（由 DXIL 反射或引擎 attributes 累加得出），
    /// 用于缓冲 stride 为 0 时兜底（D3D12 要求 StrideInBytes 非零）
    u32                         m_VertexStride = 0;
    bool                        m_NeedsZeroVB = false;
    bool                        m_NeedsInstanceIndexVB = false;   // 顶点着色器读 HE_INSTANCEINDEX
    LoadOp                      m_ColorLoadOp = LoadOp::Clear;
    LoadOp                      m_DepthLoadOp = LoadOp::Clear;
    bool                        m_DepthWrite  = true;
    std::vector<ResourceUse>    m_ResourceUses;
};

} // namespace he::rhi::d3d12

#endif // _WIN32
