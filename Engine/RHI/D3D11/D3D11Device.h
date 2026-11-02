// ============================================================================
//  D3D11Device.h —— D3D11 设备（IRHIDevice 的实现）
//
//  只有一个 immediate context（无独立计算队列、无异步计算），着色器是 SM 5.0 / DXBC，
//  没有 bindless、mesh shader、光追、DGC（能力位全部上报 false，引擎据此降级）。
//  设计与里程碑见 docs/计划实现功能/D3D11后端实现规划.md。
//
//  M2.1 / M2.2：设备创建（适配器选择、最低运行环境检查、调试层消息进日志），
//  42 个纯虚函数全部有实现；返回指针的接口一律返回非空对象（D3D12 后端“静默挂起”教训 #4）。
// ============================================================================
#pragma once

#if defined(_WIN32)

#include "D3D11Common.h"
#include "D3D11BindlessHeap.h"
#include "RHI/RHIHandles.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>   // PSO 后台预编译线程
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace he::rhi::d3d11 {

class D3D11Buffer;
class D3D11Texture;

/// 视图登记表的一项：引擎拿到的 void* 视图句柄属于哪张纹理的哪个子资源（见 D3D11Texture 的说明）
struct D3D11ViewInfo {
    enum class Type : u8 { RTV, DSV, SRV, UAV };
    D3D11Texture* texture = nullptr;
    Type          type    = Type::SRV;
    u32           mip     = 0;
    u32           layer   = 0;
    bool          single  = false;   // true：只覆盖 (mip, layer) 这一个子资源
};

class D3D11Device final : public IRHIDevice {
public:
    D3D11Device() = default;
    ~D3D11Device() override;

    /// 创建设备。不满足最低运行环境或创建失败时返回 false，原因已打日志。
    bool Initialize();

    // ---- IRHIDevice：基本信息 ----
    Backend           GetBackend() const override { return Backend::D3D11; }
    const DeviceCaps& GetCaps() const override { return m_Caps; }

    // ---- 资源创建 ----
    std::unique_ptr<IRHISwapChain>     CreateSwapChain(const SwapChainDesc& desc) override;
    std::unique_ptr<IRHICommandList>   CreateCommandList(QueueType queue = QueueType::Graphics) override;
    /// 二级列表（deferred context）放到 M7：返回空 ⇒ 引擎回退单线程录制
    std::unique_ptr<IRHICommandList>   CreateSecondaryCommandList() override { return nullptr; }
    std::unique_ptr<IRHIBuffer>        CreateBuffer(const BufferDesc& desc) override;
    std::unique_ptr<IRHITexture>       CreateTexture(const TextureDesc& desc) override;
    std::unique_ptr<IRHISampler>       CreateSampler(const SamplerDesc& desc) override;
    std::unique_ptr<IRHIPipelineState> CreatePipelineState(const PipelineStateDesc& desc) override;
    IRHIBindlessHeap*                  GetBindlessHeap() override { return &m_BindlessHeap; }
    /// ImGui 的 D3D11 后端直接往 immediate context 里画：返回已应用该命令列表状态的上下文
    void* GetImGuiCommandBuffer(IRHICommandList* cmd) const override;

    // ---- 句柄式资源（A-3）：不覆盖的话默认实现返回空句柄，引擎的句柄式资源全都拿不到 ----
    RHIBufferHandle        CreateBufferHandle(const BufferDesc& desc) override;
    void                   DestroyBufferHandle(RHIBufferHandle handle) override;
    IRHIBuffer*            ResolveBuffer(RHIBufferHandle handle) const override;
    RHITextureHandle       CreateTextureHandle(const TextureDesc& desc) override;
    void                   DestroyTextureHandle(RHITextureHandle handle) override;
    IRHITexture*           ResolveTexture(RHITextureHandle handle) const override;
    RHIPipelineStateHandle CreatePipelineStateHandle(const PipelineStateDesc& desc) override;
    void                   DestroyPipelineStateHandle(RHIPipelineStateHandle handle) override;
    IRHIPipelineState*     ResolvePipelineState(RHIPipelineStateHandle handle) const override;
    RHISamplerHandle       CreateSamplerHandle(const SamplerDesc& desc) override;
    void                   DestroySamplerHandle(RHISamplerHandle handle) override;
    IRHISampler*           ResolveSampler(RHISamplerHandle handle) const override;

    // ---- 光追：D3D11 不支持（supportsRayTracing=false，引擎不会调用）----
    std::unique_ptr<IRHIAccelerationStructure> CreateBLAS(const BLASBuildDesc&) override { return nullptr; }
    std::unique_ptr<IRHIAccelerationStructure> CreateTLAS(const TLASBuildDesc&) override { return nullptr; }
    ASBuildSizes GetBLASBuildSizes(const BLASBuildDesc&) override { return {}; }
    ASBuildSizes GetTLASBuildSizes(u32) override { return {}; }
    std::unique_ptr<IRHIRayTracingPipelineState> CreateRTPipelineState(const RTPipelineStateDesc&) override { return nullptr; }

    // ---- 描述符集 ----
    //
    //  D3D11 没有描述符集：这里只记录「binding → 资源」，不建任何 GPU 对象。
    //  命令列表在绘制 / 派发前，按当前 PSO 的绑定槽位从已绑定的集里取资源、现建视图并
    //  绑到 D3D11 槽位（见 D3D11CommandList::FlushBindings）。因此更新是「立即语义」：
    //  改了集，下一次绘制就用新内容 —— 同一帧里反复改同一个集也是合法的（规划 3.2）。
    DescriptorSetLayoutHandle CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) override;
    DescriptorSetHandle       AllocateDescriptorSet(DescriptorSetLayoutHandle layout) override;
    void DestroyDescriptorSetLayout(DescriptorSetLayoutHandle) override {}
    void UpdateDescriptorSet(DescriptorSetHandle set, u32 binding, DescriptorType type, IRHIBuffer* buffer) override;
    void UpdateDescriptorSet(DescriptorSetHandle set, u32 binding, DescriptorType type,
                             IRHITexture* texture, IRHISampler* sampler) override;
    void UpdateDescriptorSet(DescriptorSetHandle set, u32 binding, DescriptorType type,
                             IRHITexture** textures, IRHISampler** samplers, u32 count) override;
    void UpdateDescriptorSet(DescriptorSetHandle set, u32 binding, DescriptorType type,
                             IRHIBuffer** buffers, u32 count) override;
    /// 加速结构：D3D11 没有光追，忽略
    void UpdateDescriptorSet(DescriptorSetHandle, u32, DescriptorType, IRHIAccelerationStructure*) override {}
    void UpdateDescriptorSetWithImageView(DescriptorSetHandle set, u32 binding, DescriptorType type,
                                          void* imageView) override;

    /// 描述符集里一个 binding 记录的资源（数组 binding 按元素下标存）。
    /// 【写入时就持有 D3D11 视图】与 Vulkan 写描述符时拷走视图一致：引擎会在写入之后销毁纹理 /
    /// 采样器的包装对象（如局部的占位纹理），这里持有 SRV / UAV / 采样器状态的 COM 引用，
    /// 绑定时不会拿到悬空指针。缓冲的视图依赖着色器反射的步长，仍按裸指针记录、绑定时再建
    struct DescriptorEntry {
        DescriptorType                                   type = DescriptorType::UniformBuffer;
        std::vector<IRHIBuffer*>                         buffers;    // 缓冲（数组时多个）
        std::vector<ComPtr<ID3D11ShaderResourceView>>    srvs;       // 纹理的 SRV（整资源或视图句柄指定的子资源）
        std::vector<ComPtr<ID3D11UnorderedAccessView>>   uavs;       // 同一纹理的 UAV（纹理不可作 UAV 时为空）
        std::vector<ComPtr<ID3D11SamplerState>>          samplers;   // 采样器（组合图像采样器或独立采样器）
    };
    struct DescriptorSet {
        std::unordered_map<u32, DescriptorEntry> entries;   // binding → 资源
    };
    /// 供命令列表使用：句柄 → 集（无效句柄返回空）
    const DescriptorSet* FindDescriptorSet(DescriptorSetHandle set) const {
        return (set > 0 && set <= m_DescriptorSets.size()) ? &m_DescriptorSets[set - 1] : nullptr;
    }

    // ---- 逐 mip / 逐层视图 ----
    // 视图对象由纹理持有并缓存（随纹理销毁），这里只返回登记过的视图指针：
    // 存储视图对可作渲染目标的纹理返回该 mip / 层的 RTV（IBL 预滤波逐 mip 逐面渲染），否则返回 UAV
    void* CreateTextureMipStorageView(IRHITexture* texture, u32 mipLevel) override {
        return CreateTextureMipStorageView(texture, mipLevel, 0);
    }
    void* CreateTextureMipStorageView(IRHITexture* texture, u32 mipLevel, u32 arrayLayer) override;
    void* CreateTextureMipSampledView(IRHITexture* texture, u32 mipLevel) override {
        return CreateTextureMipSampledView(texture, mipLevel, 0);
    }
    void* CreateTextureMipSampledView(IRHITexture* texture, u32 mipLevel, u32 arrayLayer) override;
    /// 视图归纹理所有，销毁时什么都不用做
    void  DestroyTextureMipView(void*) override {}

    // ---- 视图登记表（纹理创建视图时登记，析构时注销）----
    void RegisterView(void* view, const D3D11ViewInfo& info) { if (view) m_Views[view] = info; }
    void UnregisterTextureViews(D3D11Texture* texture);
    /// 查视图句柄；未登记返回空
    const D3D11ViewInfo* FindView(void* view) const {
        auto it = m_Views.find(view);
        return it != m_Views.end() ? &it->second : nullptr;
    }

    // ---- 持久映射缓冲（Map 之后未 Unmap，见 D3D11Buffer 的说明）----
    void OnBufferMapped(D3D11Buffer* buffer)   { m_OpenMaps.insert(buffer); }
    void OnBufferUnmapped(D3D11Buffer* buffer) { m_OpenMaps.erase(buffer); }
    /// 绘制 / 派发前调用：把持久映射缓冲里 CPU 改过的内容上传
    void FlushOpenMaps();
    /// 提交时调用：把持久映射缓冲里 GPU 写过的内容回读
    void RefreshOpenMaps();

    // ---- PSO 预编译（M4.4）----
    //  D3D11 没有 PSO 缓存块：后台线程按队列提前建一遍着色器与状态对象（ID3D11Device 的创建接口
    //  线程安全），让驱动的着色器编译缓存先命中；建好的对象随即丢弃，主线程之后照常创建
    void  PrecompileQueuePSO(const PipelineStateDesc& desc) override;
    void  StartPSOPrecompile() override;
    float GetPSOPrecompileProgress() const override;

    // ---- 查询（M3.7）----
    std::unique_ptr<IRHIQueryPool> CreateQueryPool(u32 queryCount, QueryType type = QueryType::Timestamp) override;
    /// 时间戳一个刻度的纳秒数：由 TIMESTAMP_DISJOINT 查询给出的频率换算（初始化时测一次，之后随帧更新）
    float GetTimestampPeriod() override { return m_TimestampPeriodNs; }
    /// 每次 Present 后调用：结束当前的 TIMESTAMP_DISJOINT 查询、开始下一个，并用已完成的结果更新频率。
    /// D3D11 的时间戳只有落在 disjoint 查询之内才有效，所以始终保持一个处于活动状态
    void OnPresent();

    // ---- 提交与同步（M2.7 实现）----
    void Submit(IRHICommandList* cmdList, RHICommandList* stream = nullptr) override;
    void SubmitAll(Span<IRHICommandList*> cmdLists) override;
    void WaitIdle() override;

    /// D3D11 只有一个 immediate context：没有独立计算队列，RenderGraph 走单队列路径
    bool HasAsyncComputeQueue() const override { return false; }
    u32  GetQueueFamily(QueueType) const override { return 0; }

    // ---- 时间线围栏：ID3D11Fence（D3D11.4）----
    // 只有一个 immediate context，队列参数被忽略；GPU 侧等待在单队列下没有意义且可能死锁，不下发
    RHIFenceHandle CreateFence() override;
    void           DestroyFence(RHIFenceHandle fence) override;
    bool           WaitForFence(RHIFenceHandle fence, u64 value, u64 timeout = UINT64_MAX) override;
    u64            GetFenceValue(RHIFenceHandle fence) const override;
    void           SignalFenceOnQueue(QueueType queue, RHIFenceHandle fence, u64 value) override;
    void           WaitFenceOnQueue(QueueType queue, RHIFenceHandle fence, u64 value) override;
    ID3D11Fence*   FindFence(RHIFenceHandle fence) const {
        return (fence > 0 && fence <= m_Fences.size()) ? m_Fences[fence - 1].Get() : nullptr;
    }

    // ---- 推送常量的常量环（规划 3.5）----
    //  一块 D3D11_USAGE_DYNAMIC 的常量缓冲，按 256 字节一槽环形分配，所有命令列表共用
    //  （只有一个 immediate context）。分配时用 MAP_WRITE_NO_OVERWRITE 写入新槽，转回开头时用
    //  MAP_WRITE_DISCARD 换一块新内存（已发出的命令仍引用旧内存）。换内存后旧槽位的内容不再
    //  有效，所以带一个「代次」：命令列表记下的槽位代次与当前不同就要重新分配。
    static constexpr u32 kPushSlotSize  = 256;
    static constexpr u32 kPushSlotCount = 4096;   // 1 MB
    /// 写入一槽推送常量，返回该槽的常量起点（以 16 字节常量计，供 *SetConstantBuffers1 使用）
    bool  AllocatePushConstants(const void* data, u32& firstConstant, u64& generation);
    ID3D11Buffer* GetPushConstantRing() const { return m_PushRing.Get(); }
    u64   GetPushRingGeneration() const { return m_PushRingGeneration; }

    // ---- 内部访问（供命令列表 / 资源使用，非 RHI 接口）----
    ID3D11Device5*        GetDevice()  const { return m_Device.Get(); }
    ID3D11DeviceContext4* GetContext() const { return m_Context.Get(); }
    IDXGIFactory2*        GetFactory() const { return m_Factory.Get(); }
    /// 调试标注（RenderDoc / PIX 里显示的分组与标记）；运行时不支持时为空
    ID3DUserDefinedAnnotation* GetAnnotation() const { return m_Annotation.Get(); }

    /// immediate context 当前装着哪个命令列表的状态（多个列表共用一个上下文，见 D3D11CommandList）
    class D3D11CommandList* GetContextOwner() const { return m_ContextOwner; }
    void SetContextOwner(class D3D11CommandList* cl) { m_ContextOwner = cl; }

    // ---- 资源冲突解绑（规划 3.7）----
    //  D3D11 运行时会把同时作为输出（RTV / DSV / UAV）与输入（SRV）绑定的资源静默解绑并告警。
    //  这里记录 immediate context 上各阶段 SRV 槽位当前指向的资源（上下文是所有命令列表共用的，
    //  所以放在设备上）；绑定输出之前先把仍以 SRV 挂着同一资源的槽位清空。
    static constexpr u32 kSrvStageCount = 3;   // 与 D3D11PipelineState::Stage 的 VS / PS / CS 一致
    static constexpr u32 kSrvSlotCount  = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
    /// 记录 stage 阶段 [slot, slot + count) 新绑定的 SRV（命令列表每次下发 SRV 后调用）
    void TrackSrvs(u32 stage, u32 slot, u32 count, ID3D11ShaderResourceView* const* views);
    /// 把所有阶段里以 SRV 绑着 resources 中任一资源的槽位置空（绑定 RTV / DSV / UAV 前调用）
    void UnbindSrvsOf(ID3D11Resource* const* resources, u32 count);

    // ---- 间接参数整形（规划 3.3）----
    /// DrawIndexedIndirectCount 用：影子参数缓冲里从 argsOffset 起的 maxDraw 条命令，序号 ≥ 计数缓冲
    /// countOffset 处的值的，instanceCount 置 0（内置小 CS，在 GPU 上完成，不回读计数）。
    /// 会改动 CS 阶段的着色器与绑定：调用后清空上下文所有者，命令列表下次使用时重新应用自己的状态
    bool ClampIndirectCount(D3D11Buffer* countBuffer, u64 countOffset, ID3D11UnorderedAccessView* argsUav,
                            u64 argsOffset, u32 stride, u32 maxDraw);
    /// DrawIndexedIndirectCount 在 CPU 上要逐条调用 maxDrawCount 次，而引擎常把 maxDrawCount 设为
    /// 容量（如 Nanite 的 1M 条）。这里异步回读计数（不等待 GPU，用前几帧已完成的值），返回本次
    /// 需要循环的条数：min(maxDraw, 2 × 最近计数 + 64)；还没有回读结果时为 min(maxDraw, 65536)。
    /// 多画的命令已被 ClampIndirectCount 置为 0 实例，只影响 CPU 开销
    u32  EstimateIndirectCount(D3D11Buffer* countBuffer, u64 countOffset, u32 maxDraw);

    /// 全零顶点缓冲（步长 0）：着色器声明了、引擎顶点布局未提供的属性从这里读 0
    ID3D11Buffer* GetZeroVertexBuffer();
    /// 实例序号缓冲：第 i 个 u32 为 i，供顶点着色器取含起始实例的实例号（见 D3D11PipelineState）
    ID3D11Buffer* GetInstanceIndexBuffer();
    /// 实例序号缓冲容量（实例号上限）：覆盖对象号、Nanite 簇号与实例化网格的实例数（与 D3D12 一致）
    static constexpr u32 kInstanceIndexCount = 1u << 20;

    /// 诊断：记下最近执行的绘制 / 派发所用的 PSO 名（环形 32 条，只存指针，开销可忽略）
    void NoteExecuted(const char* psoName) { m_RecentPsos[m_RecentCursor++ % kRecentPsoCount] = psoName; }
    /// GPU 等待超时时调用：打印一次最近执行过的 PSO，便于定位是哪个 pass 让 GPU 停住
    void OnGpuTimeout();

    /// 把调试层（InfoQueue）里积累的消息写进日志并清空。ERROR / CORRUPTION 打错误，
    /// WARNING 打告警（同一消息 ID 只打前几次，避免每帧刷屏）。调试层未启用时什么都不做。
    void DrainDebugMessages();

private:
    /// 选择适配器并创建设备、检查最低运行环境（Initialize 的分解步骤，便于早退）
    bool CreateDeviceAndContext();
    /// 最低运行环境之外的能力查询，填充 m_Caps
    void QueryCaps();

    /// 句柄表：RHIResourceTable 不持有对象，这里按句柄下标另存所有权
    template <typename THandle, typename T>
    struct OwningTable {
        RHIResourceTable<THandle, T>    table;
        std::vector<std::unique_ptr<T>> owned;   // 下标 = 句柄 index - 1

        THandle Add(std::unique_ptr<T> obj) {
            if (!obj) return {};
            const THandle h = table.Add(obj.get());
            if (owned.size() < h.index) owned.resize(h.index);
            owned[h.index - 1] = std::move(obj);
            return h;
        }
        T* Get(THandle h) const { return table.Get(h); }
        void Remove(THandle h) {
            if (!table.Get(h)) return;
            table.Remove(h);
            owned[h.index - 1].reset();
        }
    };
    OwningTable<RHIBufferHandle, IRHIBuffer>               m_Buffers;
    OwningTable<RHITextureHandle, IRHITexture>             m_Textures;
    OwningTable<RHIPipelineStateHandle, IRHIPipelineState> m_PipelineStates;
    OwningTable<RHISamplerHandle, IRHISampler>             m_Samplers;

    ComPtr<IDXGIFactory2>        m_Factory;
    ComPtr<ID3D11Device5>        m_Device;
    ComPtr<ID3D11DeviceContext4> m_Context;
    ComPtr<ID3D11InfoQueue>      m_InfoQueue;     // 调试层启用时才有
    ComPtr<ID3DUserDefinedAnnotation> m_Annotation;
    ComPtr<ID3D11Buffer>         m_ZeroVertexBuffer;      // 懒创建
    ComPtr<ID3D11Buffer>         m_InstanceIndexBuffer;   // 懒创建
    class D3D11CommandList*      m_ContextOwner = nullptr;
    // 各阶段 SRV 槽位当前指向的资源（只作比较，不持有引用）与可能非空的槽位上界
    ID3D11Resource*              m_SrvResources[kSrvStageCount][kSrvSlotCount] = {};
    u32                          m_SrvHigh[kSrvStageCount] = {};
    // PSO 预编译队列（描述里的着色器字节码已深拷贝，调用方的临时对象失效后仍可用）
    struct PrecompileItem {
        PipelineStateDesc                            desc;
        std::vector<std::unique_ptr<ShaderBytecode>> shaders;
    };
    std::vector<std::unique_ptr<PrecompileItem>> m_PrecompileQueue;
    std::mutex                        m_PrecompileMutex;
    std::thread                       m_PrecompileThread;
    std::atomic<u32>                  m_PrecompileDone{ 0 };
    std::atomic<u32>                  m_PrecompileTotal{ 0 };
    std::atomic<bool>                 m_PrecompileStop{ false };
    // 时间戳的 disjoint 查询环（见 OnPresent）
    static constexpr u32              kDisjointCount = 4;
    ComPtr<ID3D11Query>               m_Disjoint[kDisjointCount];
    bool                              m_DisjointPending[kDisjointCount] = {};   // 已结束、结果还没取
    u32                               m_DisjointCursor = 0;                     // 当前活动的那个
    bool                              m_DisjointActive = false;
    float                             m_TimestampPeriodNs = 1.0f;
    /// 开始环里的下一个 disjoint 查询
    void BeginDisjoint();
    // 间接计数的异步回读（按计数缓冲 + 偏移区分）：暂存缓冲环，结果晚几帧可用
    struct CountReadback {
        static constexpr u32 kRing = 4;
        ComPtr<ID3D11Buffer> staging[kRing];
        bool                 pending[kRing] = {};
        u32                  cursor = 0;
        u32                  size   = 0;        // 暂存缓冲大小（≥ 计数所在元素）
        u32                  offset = 0;        // 计数在暂存缓冲里的偏移
        bool                 known  = false;    // 是否已有回读结果
        u32                  lastCount = 0;
    };
    std::unordered_map<u64, CountReadback> m_CountReadbacks;
    // 参数整形 CS 及其资源（第一次用到时创建）
    ComPtr<ID3D11ComputeShader>       m_ClampCS;
    bool                              m_ClampCSFailed = false;    // 编译失败过就不再重试
    ComPtr<ID3D11Buffer>              m_ClampParams;              // 16 字节动态常量缓冲
    ComPtr<ID3D11Buffer>              m_CountScratch;             // 计数所在的一段（原始视图可读）
    ComPtr<ID3D11ShaderResourceView>  m_CountScratchSrv;
    u32                               m_CountScratchSize = 0;
    static constexpr u32         kRecentPsoCount = 32;
    const char*                  m_RecentPsos[kRecentPsoCount] = {};
    u32                          m_RecentCursor = 0;
    bool                         m_TimeoutReported = false;
    D3D11BindlessHeap            m_BindlessHeap;  // 引擎会无条件解引用它
    DeviceCaps                   m_Caps{};

    std::unordered_map<void*, D3D11ViewInfo> m_Views;      // 视图句柄 → 纹理子资源
    std::unordered_set<D3D11Buffer*>         m_OpenMaps;   // 当前处于映射状态的缓冲

    /// 取（必要时创建）描述符集里的一项；无效句柄返回空
    DescriptorEntry* EntryFor(DescriptorSetHandle set, u32 binding);

    std::vector<DescriptorSetLayoutDesc> m_SetLayouts;      // 句柄 = 下标 + 1
    std::vector<DescriptorSet>           m_DescriptorSets;  // 句柄 = 下标 + 1

    std::vector<ComPtr<ID3D11Fence>> m_Fences;      // 句柄 = 下标 + 1（销毁后槽位置空）
    ComPtr<ID3D11Fence>              m_IdleFence;   // WaitIdle 专用
    u64                              m_IdleValue = 0;

    ComPtr<ID3D11Buffer> m_PushRing;
    u32                  m_PushCursor = 0;          // 下一个可用槽
    u64                  m_PushRingGeneration = 1;  // 每次 DISCARD 换内存加一
};

} // namespace he::rhi::d3d11

#endif // _WIN32
