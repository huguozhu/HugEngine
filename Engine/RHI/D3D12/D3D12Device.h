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

/// 设备丢失时打印 DRED 面包屑与页错误信息（需 HE_D3D12_DRED=1，只打印一次）
void DumpDeviceRemovedInfo(ID3D12Device* dev);
/// DumpDeviceRemovedInfo 用：在已登记的命令列表里查某次录制的事件 / 标记名
bool FindRecordedEventNames(const ID3D12CommandList* native, u32 beginCount, u32 markerCount,
                            std::vector<std::string>& begins, std::vector<std::string>& markers);
/// 按资源描述与子资源范围构造纹理的 SRV / UAV 描述（3D / 立方体 / 数组层 / 深度格式映射）
void BuildTextureViewDescs(const D3D12_RESOURCE_DESC& rd, u32 mip, u32 layer, bool single,
                           D3D12_SHADER_RESOURCE_VIEW_DESC& srv, D3D12_UNORDERED_ACCESS_VIEW_DESC& uav);

class D3D12SwapChain;   // 前置声明（设备要保存它的指针）

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
    // CBV_SRV_UAV 堆上限是 1,000,000 描述符。
    //
    // 【描述符集的写入与 GPU 读取分离】引擎按 Vulkan 语义（UPDATE_AFTER_BIND）在帧间原地
    // 改写描述符集（如 TAA 历史纹理每帧乒乓切换），而 D3D12 的 GPU 在**执行**绘制时才读
    // shader-visible 堆里的描述符：直接写 shader-visible 堆会改到仍在 GPU 队列里的帧。
    // 因此描述符集的内容只写进 CPU 侧暂存堆（不可被着色器访问，随时可改），绘制/派发前
    // 再把集的当前内容整段拷到 shader-visible 堆的环形区，根参数表指向这份快照。
    //
    // shader-visible 堆划分：
    //   [0, kSetRingBase)                         未用（描述符集只存在于暂存堆）
    //   [kSetRingBase, + 块大小 × 块数)            描述符集快照环形区，按块随围栏回收
    //   [kDescriptorHeapCapacity - 4096, 末尾)     bindless 区（最后一槽给 ImGui 字体）
    static constexpr u32 kDescriptorHeapCapacity = 524288;
    /// 描述符集在暂存堆里可用的槽位总数（集按 2×slotCount 线性分配）
    static constexpr u32 kSetRegionCapacity = 131072;
    /// 快照环形区：起点、每块槽数（必须容得下最大的集，目前约 8300）、块数
    static constexpr u32 kSetRingBase       = kSetRegionCapacity;
    static constexpr u32 kSetRingChunkSize  = 16384;
    static constexpr u32 kSetRingChunkCount = 23;
    static_assert(kSetRingBase + kSetRingChunkSize * kSetRingChunkCount <= kDescriptorHeapCapacity - 4096,
                  "快照环形区不能与堆尾的 bindless 区重叠");
    static_assert(kSetRingChunkCount <= 32, "命令列表用 32 位掩码记录用过的块");
    /// 未使用的 bindless 镜像堆的容量（与主堆脱钩，避免随主堆扩容）
    static constexpr u32 kBindlessMirrorCapacity = 65536;
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

    // ---- ImGui（imgui_impl_dx12）----
    // ImGui 的字体 SRV 放在主 CBV_SRV_UAV 堆的最后一个槽位（描述符集从 0 往上分配、
    // bindless 区只用到基址后几百个槽，二者都碰不到它），这样 ImGui 与场景共用同一张
    // 已绑定的堆，不必在帧中途切换描述符堆。
    static constexpr u32 kImGuiFontSlot = kDescriptorHeapCapacity - 1;
    /// 返回主 CBV_SRV_UAV 堆（ImGui_ImplDX12_Init 的 cbv_srv_heap 参数）
    void* CreateImGuiDescriptorPool() override;

    /// 命令列表登记（DRED 报告按原生列表指针找回各次录制的事件名）
    void RegisterCommandList(class D3D12CommandList* cl);
    void UnregisterCommandList(class D3D12CommandList* cl);

    /// 资源命名（ID3D12Object::SetName）：RenderDoc / PIX / DRED 报告里显示。采样器与加速结构没有对应的 D3D12 对象
    void SetResourceDebugName(IRHIBuffer* resource, const char* name) override;
    void SetResourceDebugName(IRHITexture* resource, const char* name) override;
    void SetResourceDebugName(IRHIPipelineState* resource, const char* name) override;
    void  DestroyImGuiDescriptorPool(void*) override {}
    /// 返回 ID3D12GraphicsCommandList*，并为外部渲染（ImGui）准备好描述符堆
    void* GetImGuiCommandBuffer(IRHICommandList* cmd) const override;
    D3D12_CPU_DESCRIPTOR_HANDLE GetImGuiFontSrvCpu() const { return GetSlotCpuHandle(kImGuiFontSlot); }
    D3D12_GPU_DESCRIPTOR_HANDLE GetImGuiFontSrvGpu() const { return GetSetGpuHandle(kImGuiFontSlot); }

    // ---- 视图句柄 -> 资源 的反查 ----
    // 引擎只把 RTV/DSV 的 CPU 句柄（void*）传给命令列表，而 D3D12 做资源屏障
    // 需要 ID3D12Resource*，所以建视图时登记一下，用时反查。
    void RegisterViewResource(UINT64 handle, ID3D12Resource* res);
    /// 登记「只指向某个子资源（mip / 数组层）」的视图句柄，对应 Vulkan 带 subresourceRange 的 ImageView
    void RegisterSubresourceView(UINT64 handle, ID3D12Resource* res, u32 mip, u32 layer);
    void UnregisterViewResource(UINT64 handle);
    ID3D12Resource* FindViewResource(UINT64 handle) const;
    /// 视图句柄 → 资源与子资源范围（UpdateDescriptorSetWithImageView 据此现建 SRV / UAV）
    struct ViewInfo {
        ID3D12Resource* res = nullptr;
        u32  mip   = 0;
        u32  layer = 0;
        bool subresource = false;   // true：只覆盖 (mip, layer) 这一个子资源
    };
    const ViewInfo* FindViewInfo(UINT64 handle) const;
    /// 深度纹理的只读 DSV（READ_ONLY_DEPTH）：同一张深度图需要同时作附件与 SRV 时使用
    void RegisterReadOnlyDsv(UINT64 dsv, UINT64 readOnlyDsv) { if (dsv && readOnlyDsv) m_ReadOnlyDsv[dsv] = readOnlyDsv; }
    void UnregisterReadOnlyDsv(UINT64 dsv) { m_ReadOnlyDsv.erase(dsv); }
    UINT64 GetReadOnlyDsv(UINT64 dsv) const { auto it = m_ReadOnlyDsv.find(dsv); return it != m_ReadOnlyDsv.end() ? it->second : 0; }
    D3D12_RESOURCE_STATES GetResourceState(ID3D12Resource* res) const;
    void SetResourceState(ID3D12Resource* res, D3D12_RESOURCE_STATES st);
    /// 分配一块常量缓冲（返回映射地址与它的 GPU 描述符句柄）
    bool AllocateConstants(D3D12_GPU_VIRTUAL_ADDRESS& outGpuAddr,
                           D3D12_GPU_DESCRIPTOR_HANDLE& outCbvHandle);

    ID3D12DescriptorHeap*            GetCbvHeap() const;
    /// 设备创建的唯一交换链（命令列表没拿到交换链时用它兜底）
    void                             SetSwapChainPtr(D3D12SwapChain* sc) { m_SwapChainPtr = sc; }
    D3D12SwapChain*                  GetSwapChainPtr() const { return m_SwapChainPtr; }

    /// 诊断：管线统计查询（判断绘制是否真的产生了图元）
    bool PassStatsBeginQuery(ID3D12GraphicsCommandList* l, const char* tag);
    void PassStatsEndQuery(ID3D12GraphicsCommandList* l);
    void PassStatsRead(const char* tag);
    /// 诊断：扫描所有 1920x1080 RGBA16F 资源，找出到底哪一张有内容
    void ScanAllHdrTargets();
    D3D12_GPU_DESCRIPTOR_HANDLE      GetCbvHeapGpuStart() const;
    void*                            GetConstantSlotPtr(D3D12_GPU_VIRTUAL_ADDRESS gpuAddr);
    D3D12_GPU_DESCRIPTOR_HANDLE      GetDefaultSamplerGpu() const;
    /// 描述符集采样器堆（槽位与 CBV_SRV_UAV 堆一致）
    // ---- bindless（set=2）----
    /// 引擎通过 bindless 句柄索引纹理/缓冲：必须真的写进 shader-visible 堆，
    /// 否则着色器读到空描述符，顶点会全部塌缩（实测 PSInvoc=0）
    /// 延迟回收：纹理可能在命令列表已录制、尚未提交时被销毁；此时若立刻释放
    /// 资源与其描述符堆，已录制的绘制会引用失效的 RTV/DSV，写入被静默丢弃。
    /// 这里把资源与堆挂到一个待回收表，等下一次 WaitIdle 之后再真正释放。
    void DeferRetire(ComPtr<ID3D12Resource> res, ComPtr<ID3D12DescriptorHeap> heap);
    void FlushRetired();

    /// 记录结构化缓冲的「元素字节数」：D3D12 的 StructuredBuffer<T> 的 SRV 必须用
    /// sizeof(T) 作为 StructureByteStride，而引擎的 BufferDesc 只给顶点/索引缓冲带 stride。
    /// 这里在 PSO 创建时从 DXIL 反射里算出每个 register 对应结构的字节数并缓存。
    /// PSO 创建时调用：从着色器反射里记下各结构化缓冲的真实步长，按「布局 + binding」存放
    ///（register space S 对应 layouts[S]）；若某 binding 的步长变化，重写该布局下已写入的缓冲描述符
    void NoteSetLayoutStrides(ID3D12ShaderReflection* refl, const std::vector<DescriptorSetLayoutHandle>& layouts);
    u32  LookupStructStride(u32 registerIndex) const;

    bool WriteBindlessSrv(u32 slot, IRHITexture* texture);
    /// 描述符堆尚未创建时，bindless 缓冲写入会被丢弃；这里排队等堆就绪后补写
    void FlushPendingBindlessBuffers();
    bool WriteBindlessBufferSrv(u32 slot, IRHIBuffer* buffer);
    /// bindless 采样器数组（space2 的 s0 起），必须写进独立的 shader-visible 采样器堆
    bool WriteBindlessSampler(u32 slot, IRHISampler* sampler);
    D3D12_GPU_DESCRIPTOR_HANDLE GetBindlessSmpGpuHandle() const;
    u32 GetBindlessSrvBase() const { return m_BindlessSrvBase; }
    u32 GetBindlessSmpBase() const { return m_BindlessSmpBase; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetBindlessSrvGpuHandle() const;
    /// 整张 CBV_SRV_UAV 堆的 GPU 基址（扁平 space0 绑定用）
    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvHeapGpuStart() const;
    ID3D12DescriptorHeap*            GetSetSamplerHeap() const { return m_SetSamplerHeap.Get(); }
    D3D12_GPU_DESCRIPTOR_HANDLE      GetSetSamplerGpuHandle(u32 slot) const;
    D3D12_GPU_DESCRIPTOR_HANDLE      GetSetGpuHandle(u32 slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE      GetSlotCpuHandle(u32 slot) const;
    /// 描述符集当前内容的版本号：每写一次加一，命令列表据此判断快照是否过期
    u64                              GetSetVersion(DescriptorSetHandle set) const;
    /// 把描述符集的当前内容（SRV/CBV 区 + UAV 区）拷进快照环形区，输出两张表的 GPU 句柄
    /// 与所用的块号；失败（无效集 / 堆未就绪）返回 false
    bool SnapshotDescriptorSet(DescriptorSetHandle set, D3D12_GPU_DESCRIPTOR_HANDLE& srvOut,
                               D3D12_GPU_DESCRIPTOR_HANDLE& uavOut, u32& chunkOut);
    /// 命令列表提交后调用：它用过的快照块（位掩码）在 fence 到达 value 前不能被覆盖
    void MarkSetRingChunks(u32 chunkMask, ID3D12Fence* fence, u64 value);
    /// 某个描述符集的堆内起始槽位（0xFFFFFFFF 表示无效句柄）
    u32                              GetSetBaseSlot(DescriptorSetHandle set) const;
    /// 取 set 某个 binding 写入的资源（纹理或 DEFAULT 堆缓冲），供绘制/派发前补状态转换；
    /// 没写过或不可转状态（如 UPLOAD 堆缓冲）时返回 nullptr。isBuffer 输出资源是否为缓冲
    ID3D12Resource* GetSetResource(DescriptorSetHandle set, u32 binding, bool& isBuffer) const;
    /// 该 set 在描述符集采样器堆中的起始槽位（采样器槽位 = 该值 + binding）
    u32                              GetSetSamplerBase(DescriptorSetHandle set) const;
    /// UAV 区域相对 baseSlot 的偏移（= 该 set 的 slotCount）
    u32                              GetSetUavOffset(DescriptorSetHandle set) const;

    /// 指定 mip（+ 数组层）的附件视图。D3D12 下对可作渲染目标的纹理返回该 mip/层的 RTV
    ///（IBL 预滤波逐 mip 逐面渲染用）；其它纹理（UAV 用途）暂不支持，返回 nullptr
    void* CreateTextureMipStorageView(IRHITexture* texture, u32 mipLevel) override {
        return CreateTextureMipStorageView(texture, mipLevel, 0);
    }
    void* CreateTextureMipStorageView(IRHITexture* texture, u32 mipLevel, u32 arrayLayer) override;
    void* CreateTextureMipSampledView(IRHITexture* texture, u32 mipLevel) override {
        return CreateTextureMipSampledView(texture, mipLevel, 0);
    }
    void* CreateTextureMipSampledView(IRHITexture* texture, u32 mipLevel, u32 arrayLayer) override;
    void  DestroyTextureMipView(void* view) override;

    /// 每个时间戳 tick 的纳秒数：D3D12 时间戳以 tick 计，频率由队列给出（此前误按 1ns 返回 1.0）
    float GetTimestampPeriod() override;

    /// 查询池（时间戳 / 管线统计）：ID3D12QueryHeap + 持久映射的回读缓冲
    std::unique_ptr<IRHIQueryPool> CreateQueryPool(u32 queryCount,
                                                   QueryType type = QueryType::Timestamp) override;

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

    // 采样器句柄：此前漏实现，默认实现返回空句柄 ⇒ 所有 CreateSamplerHandle 创建的采样器都是空
    //（TAA 输出采样器为空 ⇒ 色调映射输入从未写入 ⇒ 延迟渲染样例全黑；Hi-Z、GI 等同理）
    RHISamplerHandle       CreateSamplerHandle(const SamplerDesc& desc) override;
    void                   DestroySamplerHandle(RHISamplerHandle handle) override;
    IRHISampler*           ResolveSampler(RHISamplerHandle handle) const override;

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
    // 【非持有】此前存的是 ComPtr，会强引用住每张纹理 —— 引擎会创建大量
    // 1920x1080 的 HDR 目标（实测 12 秒 40 张），这既造成内存泄漏，
    // 又让句柄映射指向早已不应该存在的实例。改为裸指针，纹理析构时注销。
    std::unordered_map<u32, u32> m_StructStrides;
    std::vector<std::pair<u32, IRHIBuffer*>> m_PendingBindlessBufs;
public:
    /// 【多缓冲错位修正】引擎按飞行帧写物体缓冲，但绑定的描述符集可能指向另一个
    /// 飞行帧的缓冲（实测写入地址 390594560，绑定地址 390070272，从未被写过）。
    /// 这里记录「最近一次上传的物体缓冲」，绘制前重新写进当前集的 t2 槽位。
    void SetLastObjectBuffer(class D3D12Buffer* b) { m_LastUploadedObject = b; }
    class D3D12Buffer* GetLastObjectBuffer() const { return m_LastUploadedObject; }
    /// 全零顶点缓冲视图（步长 0）：供 PSO 中引擎布局未提供的属性读取 0
    D3D12_VERTEX_BUFFER_VIEW GetZeroVertexBufferView();
private:
    class D3D12Buffer* m_LastUploadedObject = nullptr;
    ComPtr<ID3D12Resource> m_ZeroVertexBuffer;   // 懒创建的 64 字节全零缓冲
    u32                    m_HighestShaderModel = 0;   // D3D_SHADER_MODEL（如 0x68 = SM 6.8）
    // 逐 mip RTV 池（CreateTextureMipStorageView 分配、DestroyTextureMipView 回收）
    static constexpr u32 kMipRtvCapacity = 1024;
    ComPtr<ID3D12DescriptorHeap> m_MipRtvHeap;
    UINT                         m_MipRtvInc  = 0;
    u32                          m_MipRtvNext = 0;
    std::vector<u32>             m_MipRtvFree;
    /// 非渲染目标纹理的逐 mip 视图池（CPU 侧 CBV_SRV_UAV 堆）：句柄只作「视图 → 子资源」的键，
    /// 真正绑定时由 UpdateDescriptorSetWithImageView 按子资源现建 SRV / UAV
    ComPtr<ID3D12DescriptorHeap> m_MipViewHeap;
    UINT                         m_MipViewInc  = 0;
    u32                          m_MipViewNext = 0;
    std::vector<u32>             m_MipViewFree;
    /// 从逐 mip 视图池分配一个子资源视图句柄（SRV 或 UAV），失败返回 nullptr
    void* CreateMipViewKey(ID3D12Resource* res, u32 mipLevel, u32 arrayLayer, bool uav);
public:
    /// 最近一次上传的物体矩阵副本（前 32 个元素的缩放与平移），用于诊断索引是否错配
    float m_LastObjM[32][8] = {};
public:
    /// 引擎为每个描述符集指定的物体缓冲（UpdateDescriptorSet binding==2 时记录）。
    /// 绑定时用它把 SRV 写回 t2 槽位，避免跨飞行帧的启发式猜测。
    std::unordered_map<UINT64, class D3D12Buffer*> m_ObjectBufferBySet;
private:
    unsigned m_LastObjCount = 0;
private:
private:   // register -> 元素字节数
    std::unordered_map<UINT64, ViewInfo> m_ViewResources;
    std::unordered_map<UINT64, UINT64>          m_ReadOnlyDsv;   // 普通 DSV → 只读 DSV
    mutable std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> m_ResStates;

    D3D12SwapChain*              m_SwapChainPtr = nullptr;

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
    ComPtr<ID3D12DescriptorHeap> m_BindlessSrvHeap;
    ComPtr<ID3D12DescriptorHeap> m_BindlessSmpHeap;
    UINT                         m_BindlessSmpSize = 0;
    std::vector<std::pair<ComPtr<ID3D12Resource>, ComPtr<ID3D12DescriptorHeap>>> m_Retired;
    UINT                         m_BindlessDescSize = 0;
    // bindless 占用「已绑定堆」里的保留区：D3D12 同一时刻只能各绑一个 SRV 堆/采样器堆，
    // 用独立堆会在切换时让 GPU 卡死，因此改为在 m_CbvHeap / m_SetSamplerHeap 内保留一段。
    u32                          m_BindlessSrvBase = 0;
    u32                          m_BindlessSmpBase = 0;
    UINT                         m_CbvIncrement = 0;
    u32                          m_NextCbvSlot  = 0;


    UINT                         m_CbvSrvUavSize = 0;
    u32                          m_NextFreeSlot  = 0;      // 简单的线性分配（阶段 2 换空闲链表）
    /// 描述符集暂存堆（CPU 侧，不可被着色器访问）：UpdateDescriptorSet 只写这里
    ComPtr<ID3D12DescriptorHeap> m_SetStagingHeap;
    /// 快照环形区的分配位置与各块的占用记录
    struct SetRingChunk {
        /// 用过本块、已提交的命令列表的完成条件（同一围栏只保留最大值）
        std::vector<std::pair<ComPtr<ID3D12Fence>, u64>> uses;
    };
    SetRingChunk                 m_SetRingChunks[kSetRingChunkCount];
    u32                          m_SetRingChunk  = 0;      // 当前分配块
    u32                          m_SetRingOffset = 0;      // 当前块内已用槽数

    struct D3D12SetLayout {
        std::vector<DescriptorSetLayoutBinding> bindings;
        u32 slotCount = 0;   // 该布局占用的连续槽位数
        u32 samplerSpan = 0; // 采样器类 binding 的最大编号 + 1（每个 set 在采样器堆里占这么多槽）
        std::unordered_map<u32, u32> strides;   // binding → 结构化缓冲步长（来自 PSO 反射）
    };
    std::vector<D3D12SetLayout> m_SetLayouts;      // 1-based 句柄

    struct D3D12Set {
        u32 baseSlot = 0;   // SRV/CBV 区域起点
        u32 uavBase  = 0;   // UAV 区域起点（= baseSlot + slotCount）
        u32 slotCount = 0;
        u32 layout   = 0;
        u32 samplerBase = 0; // 采样器堆中的起始槽位（各 set 独立，互不覆盖）
        u64 version = 0;     // 内容版本：每次写描述符加一（快照是否过期的依据）
        /// binding → 写入的缓冲：布局学到新步长时据此重写描述符。
        /// 持有资源引用而不是引擎的缓冲指针：引擎可能已销毁/重建该缓冲，裸指针会悬空
        struct BufferBinding {
            ComPtr<ID3D12Resource> resource;
            u64                    size = 0;
            u32                    bufferStride = 0;
            DescriptorType         type = DescriptorType::StorageBuffer;
            bool                   defaultHeap = false;   // 只有 DEFAULT 堆上的缓冲能做状态转换
        };
        std::unordered_map<u32, BufferBinding> buffers;
        /// binding → 写入的纹理资源：绘制/派发前据此补状态转换
        std::unordered_map<u32, ComPtr<ID3D12Resource>> textures;
    };
    /// 按所属布局记录的步长写某个 set 某个 binding 的缓冲描述符（SRV/CBV + UAV）
    void WriteSetBufferDescriptor(D3D12Set& s, u32 binding, const D3D12Set::BufferBinding& bb);
    /// 取暂存堆槽位的 CPU 句柄，并把该集的版本号加一（所有写集描述符的地方都经过这里）
    D3D12_CPU_DESCRIPTOR_HANDLE SetWriteHandle(D3D12Set& s, u32 slot);
    /// 采样器堆下一个可分配槽位（描述符集区间从 0 往上，bindless 区在堆尾）
    u32 m_NextSamplerSlot = 0;
    std::vector<D3D12Set> m_DescriptorSets;        // 1-based 句柄

    D3D12BindlessHeap              m_BindlessHeap{ this };   // 引擎会无条件解引用它

    HandleTable<IRHIBuffer>        m_Buffers;
    HandleTable<IRHITexture>       m_Textures;
    HandleTable<IRHIPipelineState> m_PipelineStates;
    HandleTable<IRHISampler>       m_Samplers;

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
    ComPtr<ID3D12QueryHeap>      m_StatsHeap;
    int                          m_StatsSlot = 0;
    int                          m_StatsCount = 0;
    bool                         m_StatsOpen = false;
    const char*                  m_StatsTags[64] = {};
    ComPtr<ID3D12Resource>       m_StatsBuf;
    bool                         m_StatsPending = false;
};

} // namespace he::rhi::d3d12

#endif // _WIN32
