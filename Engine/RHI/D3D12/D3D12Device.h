// ============================================================================
//  D3D12Device.h —— D3D12 设备（IRHIDevice 的实现）
//
//  阶段 0 的范围：设备创建、命令队列、围栏、交换链、Buffer/Texture/Sampler 创建。
//  尚未实现的能力（光追、描述符集、bindless、mesh shader）先返回 nullptr，
//  由后续阶段补齐；调用方（引擎）在能力查询阶段会跳过它们。
// ============================================================================
#pragma once

#if defined(_WIN32)

#include "D3D12Common.h"
#include "D3D12BindlessHeap.h"
#include <vector>
#include <unordered_map>
#include <memory>

namespace he::rhi::d3d12 {

class D3D12Device final : public IRHIDevice {
public:
    D3D12Device() = default;
    ~D3D12Device() override;

    /// 创建设备。失败时返回 false，错误已打日志。
    bool Initialize();

    // ---- IRHIDevice ----
    Backend           GetBackend() const override { return Backend::D3D12; }
    const DeviceCaps& GetCaps() const override { return m_Caps; }

    std::unique_ptr<IRHISwapChain>   CreateSwapChain(const SwapChainDesc& desc) override;
    std::unique_ptr<IRHICommandList> CreateCommandList(QueueType queue = QueueType::Graphics) override;
    std::unique_ptr<IRHICommandList> CreateSecondaryCommandList() override;

    std::unique_ptr<IRHIBuffer>  CreateBuffer(const BufferDesc& desc) override;
    std::unique_ptr<IRHITexture> CreateTexture(const TextureDesc& desc) override;
    std::unique_ptr<IRHISampler> CreateSampler(const SamplerDesc& desc) override;

    std::unique_ptr<IRHIPipelineState> CreatePipelineState(const PipelineStateDesc& desc) override;
    IRHIBindlessHeap* GetBindlessHeap() override { return &m_BindlessHeap; }

    // ---- 光追（阶段 4）----
    std::unique_ptr<IRHIAccelerationStructure> CreateBLAS(const BLASBuildDesc&) override { return nullptr; }
    std::unique_ptr<IRHIAccelerationStructure> CreateTLAS(const TLASBuildDesc&) override { return nullptr; }
    ASBuildSizes GetBLASBuildSizes(const BLASBuildDesc&) override { return {}; }
    ASBuildSizes GetTLASBuildSizes(u32) override { return {}; }
    std::unique_ptr<IRHIRayTracingPipelineState> CreateRTPipelineState(const RTPipelineStateDesc&) override { return nullptr; }

    // ---- 描述符集 ----
    //
    //  D3D12 没有 Vulkan 的「描述符集」概念，只有描述符堆 + 根签名参数。
    //  映射方式：
    //    · 一个 **shader-visible CBV_SRV_UAV 堆**，容量 kDescriptorHeapCapacity
    //    · 每个 DescriptorSet 占一段连续槽位（槽位下标 = 堆基址 + binding）
    //    · BindDescriptorSet(setIndex, set) 把这段槽位作为根签名的描述符表绑定
    //  这样引擎侧「按 binding 号更新/绑定」的语义在 D3D12 上得以保留。
    // 【容量】引擎的部分描述符集 layout 里有很大的 binding 号（bindless 数组，
    // 单个 layout 就要 4000+ 槽），4096 会很快耗尽。D3D12 的 shader-visible
    // CBV_SRV_UAV 堆上限是 1,000,000 描述符，这里取 65536（约 2 MB）。
    static constexpr u32 kDescriptorHeapCapacity = 65536;
    /// shader-visible 的**采样器**堆上限是 4080（D3D12 硬限制），
    /// 用 4096 会报 "NumDescriptors 4096, but the limit ... is 4080"。
    static constexpr u32 kSetSamplerCapacity = 2048;

    DescriptorSetLayoutHandle CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) override;
    DescriptorSetHandle       AllocateDescriptorSet(DescriptorSetLayoutHandle layout) override;
    void DestroyDescriptorSetLayout(DescriptorSetLayoutHandle layout) override;
    void UpdateDescriptorSet(DescriptorSetHandle set, u32 binding, DescriptorType type, IRHIBuffer* buffer) override;
    void UpdateDescriptorSet(DescriptorSetHandle set, u32 binding, DescriptorType type, IRHITexture* texture, IRHISampler* sampler) override;
    void UpdateDescriptorSet(DescriptorSetHandle set, u32 binding, DescriptorType type, IRHITexture** textures, IRHISampler** samplers, u32 count) override;
    void UpdateDescriptorSet(DescriptorSetHandle set, u32 binding, DescriptorType type, IRHIBuffer** buffers, u32 count) override;
    void UpdateDescriptorSet(DescriptorSetHandle set, u32 binding, DescriptorType type, IRHIAccelerationStructure* as) override;
    void UpdateDescriptorSetWithImageView(DescriptorSetHandle set, u32 binding, DescriptorType type, void* imageView) override;

    /// 供 CommandList 使用：某个 set 的堆内起始槽位与句柄
    ID3D12DescriptorHeap*            GetCbvSrvUavHeap() const { return m_CbvSrvUavHeap.Get(); }
    ID3D12DescriptorHeap*            GetSamplerHeap()   const { return m_SamplerHeap.Get(); }
    /// 确保两个 shader-visible 堆都已创建（懒初始化）
    bool                             EnsureDescriptorHeaps();
    /// 分配一块常量缓冲（返回映射地址与它的 GPU 描述符句柄）
    bool AllocateConstants(D3D12_GPU_VIRTUAL_ADDRESS& outGpuAddr,
                           D3D12_GPU_DESCRIPTOR_HANDLE& outCbvHandle);

    ID3D12DescriptorHeap*            GetCbvHeap() const;
    D3D12_GPU_DESCRIPTOR_HANDLE      GetCbvHeapGpuStart() const;
    void*                            GetConstantSlotPtr(D3D12_GPU_VIRTUAL_ADDRESS gpuAddr);
    D3D12_GPU_DESCRIPTOR_HANDLE      GetDefaultSamplerGpu() const;
    /// 描述符集采样器堆（槽位与 CBV_SRV_UAV 堆一致）
    ID3D12DescriptorHeap*            GetSetSamplerHeap() const { return m_SetSamplerHeap.Get(); }
    D3D12_GPU_DESCRIPTOR_HANDLE      GetSetSamplerGpuHandle(u32 slot) const;
    D3D12_GPU_DESCRIPTOR_HANDLE      GetSetGpuHandle(u32 slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE      GetSlotCpuHandle(u32 slot) const;
    /// 某个描述符集的堆内起始槽位（0xFFFFFFFF 表示无效句柄）
    u32                              GetSetBaseSlot(DescriptorSetHandle set) const;
    /// UAV 区域相对 baseSlot 的偏移（= 该 set 的 slotCount）
    u32                              GetSetUavOffset(DescriptorSetHandle set) const;

    void* CreateTextureMipStorageView(IRHITexture*, u32) override { return nullptr; }
    void* CreateTextureMipStorageView(IRHITexture*, u32, u32) override { return nullptr; }
    void* CreateTextureMipSampledView(IRHITexture*, u32) override { return nullptr; }
    void* CreateTextureMipSampledView(IRHITexture*, u32, u32) override { return nullptr; }
    void  DestroyTextureMipView(void*) override {}

    float GetTimestampPeriod() override { return 1.0f; }   // D3D12 的 GPU 时间戳单位是 1ns

    // 查询池（时间戳/遮挡）：阶段 3 实现真正的 ID3D12QueryHeap，当前返回空
    std::unique_ptr<IRHIQueryPool> CreateQueryPool(u32 queryCount,
                                                   QueryType type = QueryType::Timestamp) override {
        (void)queryCount; (void)type;
        return nullptr;
    }

    void WaitIdle() override;
    bool HasAsyncComputeQueue() const override { return false; }
    u32  GetQueueFamily(QueueType) const override { return 0; }

    RHIFenceHandle CreateFence() override;
    void           DestroyFence(RHIFenceHandle fence) override;
    bool           WaitForFence(RHIFenceHandle fence, u64 value, u64 timeout = UINT64_MAX) override;
    u64            GetFenceValue(RHIFenceHandle fence) const override;
    void           SignalFenceOnQueue(QueueType queue, RHIFenceHandle fence, u64 value) override;
    void           WaitFenceOnQueue(QueueType queue, RHIFenceHandle fence, u64 value) override;

    void Submit(IRHICommandList* cmdList, RHICommandList* stream = nullptr) override;
    void SubmitAll(Span<IRHICommandList*> cmdLists) override;

    // ---- 内部访问（供 CommandList / SwapChain 使用，非 RHI 接口）----
    ID3D12Device*                GetDevice()  const { return m_Device.Get(); }
    ID3D12CommandQueue*          GetQueue()   const { return m_Queue.Get(); }
    UINT                         GetRtvIncrement() const { return m_RtvDescSize; }
    UINT                         GetDsvIncrement() const { return m_DsvDescSize; }
    IDXGIFactory4*               GetFactory() const { return m_Factory.Get(); }
    ID3D12InfoQueue*             GetInfoQueue() const { return m_InfoQueue.Get(); }
    void                         ReportDeviceRemoved(const char* where);

    // ---- 句柄式资源表（引擎的主力 API）----
    //
    //  引擎通过 `Create*Handle` 拿到 {index, generation} 句柄，再用 `Resolve*`
    //  换回接口指针。句柄 index==0 表示无效，所以表下标从 1 开始。
    //  这些方法是 IRHIDevice 上的**带默认实现的虚函数**（返回空句柄），
    //  不覆盖就会让所有 Create*Handle 拿到无效句柄 —— 实测 CSM 阴影贴图
    //  就是这样失败的（CreateTextureHandle 返回 {} ⇒ IsValid() == false）。
    RHIBufferHandle        CreateBufferHandle(const BufferDesc& desc) override;
    void                   DestroyBufferHandle(RHIBufferHandle handle) override;
    IRHIBuffer*            ResolveBuffer(RHIBufferHandle handle) const override;

    RHITextureHandle       CreateTextureHandle(const TextureDesc& desc) override;
    void                   DestroyTextureHandle(RHITextureHandle handle) override;
    IRHITexture*           ResolveTexture(RHITextureHandle handle) const override;

    RHIPipelineStateHandle CreatePipelineStateHandle(const PipelineStateDesc& desc) override;
    void                   DestroyPipelineStateHandle(RHIPipelineStateHandle handle) override;
    IRHIPipelineState*     ResolvePipelineState(RHIPipelineStateHandle handle) const override;

private:
    /// 通用的槽位表（index 0 保留为无效）
    template <typename T>
    struct HandleSlot {
        std::unique_ptr<T> object;
        u32                generation = 1;   // 每次分配递增，用于校验旧句柄
    };
    template <typename T>
    struct HandleTable {
        std::vector<HandleSlot<T>> slots;    // slots[0] 不用
        std::vector<u32>           freeList;

        /// 取得一个槽位，返回 1-based 下标
        u32 Acquire(std::unique_ptr<T> obj, u32& outGeneration) {
            if (slots.empty()) { slots.emplace_back(); }   // 占位 0 号
            u32 idx;
            if (!freeList.empty()) {
                idx = freeList.back();
                freeList.pop_back();
                slots[idx].object = std::move(obj);
                slots[idx].generation += 1;
            } else {
                HandleSlot<T> s;
                s.object = std::move(obj);
                slots.push_back(std::move(s));
                idx = static_cast<u32>(slots.size() - 1);
            }
            outGeneration = slots[idx].generation;
            return idx;
        }
        T* Get(u32 index, u32 generation) const {
            if (index == 0 || index >= slots.size()) return nullptr;
            if (slots[index].generation != generation) return nullptr;   // 过期句柄
            return slots[index].object.get();
        }
        void Release(u32 index, u32 generation) {
            if (index == 0 || index >= slots.size()) return;
            if (slots[index].generation != generation) return;
            slots[index].object.reset();
            slots[index].generation += 1;
            freeList.push_back(index);
        }
    };

    // ---- 描述符堆与布局表 ----
    ComPtr<ID3D12DescriptorHeap> m_CbvSrvUavHeap;
    /// 采样器堆：根签名声明了 s0..s15 的 SAMPLER 表，D3D12 要求
    /// 绑定时同时设置 CBV_SRV_UAV 与 SAMPLER 两个 shader-visible 堆，
    /// 缺一个就会让 CreateGraphicsPipelineState 返回 E_INVALIDARG。
    ComPtr<ID3D12DescriptorHeap> m_SamplerHeap;

    /// 描述符集的**采样器**堆：与 CBV_SRV_UAV 堆使用**相同的槽位编号**，
    /// 这样 set 的 baseSlot 在两个堆里都指向同一组索引。
    /// 必须单独建堆，因为 D3D12 的 SRV 堆放不了采样器。
    ComPtr<ID3D12DescriptorHeap> m_SetSamplerHeap;
    UINT                         m_SetSamplerSize = 0;

    // ---- 常量缓冲环（替代 root constants）----
    //
    //  【为什么不能只用 root constants】着色器里的 cbuffer 是**多个**
    //  （例如 PBR.frag 同时有 b0 的 GIBlendParams 和 b1 的 FrameConstants），
    //  而 root constants 只能占一个寄存器，且与 CBV 的类型不兼容
    //  ⇒ CreateGraphicsPipelineState 返回 E_INVALIDARG。
    //  这里用一个**巨大的上传缓冲**做环形分配：每次 SetPushConstants 取一块
    //  256 字节对齐的区域，并把对应 CBV 写进 CBV 堆，再绑整张表。
    static constexpr u32 kConstantSlotCount = 4096;      // 4096 × 256B = 1 MB
    static constexpr u32 kConstantSlotSize  = 256;
    ComPtr<ID3D12Resource>       m_ConstantRing;
    void*                        m_ConstantRingMapped = nullptr;
    ComPtr<ID3D12DescriptorHeap> m_CbvHeap;
    UINT                         m_CbvIncrement = 0;
    u32                          m_NextCbvSlot  = 0;


    UINT                         m_CbvSrvUavSize = 0;
    u32                          m_NextFreeSlot  = 0;      // 简单的线性分配（阶段 2 换空闲链表）

    struct D3D12SetLayout {
        std::vector<DescriptorSetLayoutBinding> bindings;
        u32 slotCount = 0;   // 该布局占用的连续槽位数
    };
    std::vector<D3D12SetLayout> m_SetLayouts;      // 1-based 句柄

    struct D3D12Set {
        u32 baseSlot = 0;   // SRV/CBV 区域起点
        u32 uavBase  = 0;   // UAV 区域起点（= baseSlot + slotCount）
        u32 slotCount = 0;
        u32 layout   = 0;
    };
    std::vector<D3D12Set> m_DescriptorSets;        // 1-based 句柄

    D3D12BindlessHeap              m_BindlessHeap{ this };   // 引擎会无条件解引用它

    HandleTable<IRHIBuffer>        m_Buffers;
    HandleTable<IRHITexture>       m_Textures;
    HandleTable<IRHIPipelineState> m_PipelineStates;

    /// 创建设备、队列、围栏（Initialize 的分解步骤，便于早退）
    bool CreateDeviceAndQueue();
    bool CreateDebugLayer();

    ComPtr<IDXGIFactory4>       m_Factory;
    ComPtr<ID3D12Device>        m_Device;
    ComPtr<ID3D12CommandQueue>  m_Queue;
    ComPtr<ID3D12Debug>         m_Debug;
    ComPtr<ID3D12InfoQueue>     m_InfoQueue;

    DeviceCaps m_Caps{};
    UINT       m_RtvDescSize = 0;
    UINT       m_DsvDescSize = 0;
    UINT       m_SrvDescSize = 0;

    // 围栏池：句柄是 1-based 下标（0 表示无效）
    struct FenceEntry {
        ComPtr<ID3D12Fence> fence;
        u64                 value = 0;
    };
    std::vector<FenceEntry> m_Fences;
    std::vector<u32>        m_FreeFences;
};

} // namespace he::rhi::d3d12

#endif // _WIN32
