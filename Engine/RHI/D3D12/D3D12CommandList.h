// ============================================================================
//  D3D12CommandList.h —— D3D12 命令列表
//
//  设计要点：
//    · D3D12 的命令列表由「命令分配器 + 列表」组成，分配器不能与正在执行的列表
//      共用 ⇒ 每个命令列表持有自己的分配器。多帧并行时由 FrameRetireQueue 负责
//      在 GPU 完成后重置（阶段 3）。
//    · 引擎把 IRHITexture 的 native handle 当 void* 传来传去，D3D12 里这个 void*
//      就是 **RTV/DSV 描述符的 CPU 句柄**（编码成指针值）。BeginOffscreenPass 里
//      需要把它还原成 D3D12_CPU_DESCRIPTOR_HANDLE。
//    · 引擎没有 render pass 概念，D3D12 也没有 ⇒ Begin*Pass 只是设置渲染目标并清屏，
//      End*Pass 什么都不做。资源状态转换靠显式 PipelineBarrier。
// ============================================================================
#pragma once

#if defined(_WIN32)

#include "D3D12Common.h"
#include <vector>

namespace he::rhi::d3d12 {

class D3D12Device;
class D3D12PipelineState;   // 前置声明：SetPipeline 里只存指针

class D3D12CommandList final : public IRHICommandList {
public:
    ~D3D12CommandList() override;

    bool Initialize(D3D12Device* device, QueueType queue);

    // ---- 生命周期 ----
    void Begin() override;
    void End()   override;
    void BeginSecondary(IRHIPipelineState*) override {}
    void ExecuteSecondary(IRHICommandList*) override {}
    bool IsSecondary() const override { return false; }

    // ---- Render pass ----
    void BeginRenderPass(u32 colorCount, Format colorFormat,
                         Format depthFormat = Format::Unknown,
                         const ClearValue* clear = nullptr,
                         LoadOp loadOp = LoadOp::Clear) override;
    void EndRenderPass() override;
    void BeginOffscreenPass(void* colorImageView, void* depthImageView,
                            u32 width, u32 height,
                            const ClearValue* clear = nullptr,
                            bool allowSecondary = false) override;
    void BeginOffscreenPassMRT(void* const* colorImageViews, u32 colorCount,
                               void* depthImageView, u32 width, u32 height,
                               const ClearValue* clears = nullptr,
                               bool allowSecondary = false) override;
    void EndOffscreenPass() override;
    void SetSwapChain(IRHISwapChain* sc) override { m_SwapChain = sc; }

    // ---- 管线与资源绑定 ----
    void SetPipeline(IRHIPipelineState* pso) override;
    void SetVertexBuffer(IRHIBuffer* buffer, u32 binding = 0) override;
    void SetIndexBuffer(IRHIBuffer* buffer, u32 offset = 0) override;
    void SetViewport(const Viewport& vp) override;
    void SetScissor(const ScissorRect& sc) override;
    void SetFullscreenViewport(u32 width, u32 height) override;
    void BindDescriptorSet(u32 setIndex, DescriptorSetHandle set) override;
    void SetPushConstants(u32 offset, u32 size, const void* data) override;

    // ---- 绘制 ----
    void Draw(u32 vertexCount, u32 instanceCount = 1, u32 firstVertex = 0, u32 firstInstance = 0) override;
    void DrawIndexed(u32 indexCount, u32 instanceCount = 1, u32 firstIndex = 0,
                     i32 vertexOffset = 0, u32 firstInstance = 0) override;
    void DrawIndexedIndirect(IRHIBuffer* buffer, u64 offset, u32 drawCount, u32 stride) override;
    void DrawIndexedIndirectCount(IRHIBuffer* buffer, u64 offset, IRHIBuffer* countBuffer,
                                  u64 countOffset, u32 maxDrawCount, u32 stride) override;
    void DrawMeshTasks(u32, u32 = 1, u32 = 1) override {}
    void DrawMeshTasksIndirect(IRHIBuffer*, u64, u32, u32) override {}
    void DrawMeshTasksIndirectCount(IRHIBuffer*, u64, IRHIBuffer*, u64, u32, u32) override {}

    // ---- 计算 ----
    void Dispatch(u32 x, u32 y, u32 z) override;
    void DispatchIndirect(IRHIBuffer* buffer, u64 offset) override;

    // ---- 同步 ----
    void PipelineBarrier(PipelineStage srcStage, PipelineStage dstStage,
                         ResourceState srcState, ResourceState dstState) override;
    void PipelineBarrier(PipelineStage srcStage, PipelineStage dstStage,
                         ResourceState srcState, ResourceState dstState,
                         IRHITexture* texture) override;

    // ---- 拷贝 ----
    void CopyBuffer(IRHIBuffer* src, IRHIBuffer* dst, u64 size,
                    u64 srcOffset = 0, u64 dstOffset = 0) override;
    void CopyTextureToTexture(IRHITexture* src, IRHITexture* dst) override;
    /// 纹理 (mip 0, 层 0) 的矩形区域 → 缓冲（从 bufferOffset 起逐行紧密排列，与 Vulkan 的
    /// bufferRowLength=0 一致）。width / height 为 0 表示取满纹理
    void CopyTextureToBuffer(IRHITexture* src, IRHIBuffer* dst, u32 x = 0, u32 y = 0,
                             u32 width = 0, u32 height = 0, u64 bufferOffset = 0) override;

    // ---- 光追（阶段 4）----
    void BuildBLAS(IRHIAccelerationStructure*, IRHIBuffer*, const BLASBuildDesc&, bool = false) override {}
    void BuildTLAS(IRHIAccelerationStructure*, IRHIBuffer*, IRHIBuffer*, u32, bool = false) override {}
    void BindRTPipeline(IRHIRayTracingPipelineState*) override {}
    void TraceRays(const SBTDesc&, u32, u32, u32 = 1) override {}

    // ---- 队列归属（Vulkan 专有，D3D12 无对应概念）----
    void QueueOwnershipTransfer(IRHITexture*, QueueType, QueueType, ResourceState, ResourceState) override {}
    void QueueOwnershipTransfer(IRHIBuffer*, QueueType, QueueType, ResourceState, ResourceState) override {}
    void ReleaseToQueue(IRHITexture*, QueueType) override {}
    void AcquireFromQueue(IRHITexture*, QueueType) override {}

    // ---- 时间线与查询 ----
    void SetTimelineSignal(RHIFenceHandle fence, u64 value) override;
    void SetTimelineWait(RHIFenceHandle fence, u64 value) override;
    // GPU 查询：EndQuery 后立即 Resolve 到回读缓冲，提交时打围栏，读取时按围栏判断可用
    void WriteTimestamp(IRHIQueryPool* pool, u32 queryIndex) override;
    void ResetQueryPool(IRHIQueryPool* pool) override;
    void GetQueryResults(IRHIQueryPool* pool, u32 first, u32 count, u64* data) override;
    bool TryGetQueryResults(IRHIQueryPool* pool, u32 first, u32 count, u64* data) override;
    void BeginQuery(IRHIQueryPool* pool, u32 queryIndex) override;
    void EndQuery(IRHIQueryPool* pool, u32 queryIndex) override;

    void Submit() override;

    /// 供 Device::Submit 使用
    ID3D12GraphicsCommandList* GetNativeCommandList() const { return m_List.Get(); }
    /// 提交到队列后调用：在队列上打一个围栏，标记当前帧槽位的分配器何时可以复用
    void OnSubmitted(ID3D12CommandQueue* queue);

    // ---- 调试标签（PIX 事件格式：RenderDoc / PIX 显示为分组与标记，DRED 报告据此给出所在 pass）----
    void BeginDebugLabel(const char* name, const float color[4] = nullptr) override;
    void EndDebugLabel() override;
    void InsertDebugLabel(const char* name, const float color[4] = nullptr) override;
    /// 记下标签，下一次绘制 / 派发前插入一次瞬时标记（与 Vulkan 一致，受 r.Debug.DrawMarker 控制）
    void SetDrawDebugLabel(const char* name, const float color[4] = nullptr) override;
    void ClearDrawDebugLabel() override;
    /// 提交前 / 后调用：执行录制期记下的时间线等待 / 信号（见 SetTimelineWait / SetTimelineSignal）
    void ApplyTimelineWaits();
    void ApplyTimelineSignals();
    /// 外部代码（ImGui）直接往原生列表录制前调用：绑定引擎的描述符堆；
    /// 外部会改根签名与 PSO，因此之后的绘制必须重新 SetPipeline 并下发根参数
    void PrepareForExternalRendering();

private:
    /// 把引擎传来的 void* 还原成 D3D12 的 CPU 描述符句柄
    static D3D12_CPU_DESCRIPTOR_HANDLE ToCpuHandle(void* p) {
        D3D12_CPU_DESCRIPTOR_HANDLE h{};
        h.ptr = reinterpret_cast<SIZE_T>(p);
        return h;
    }

public:
    u32 GetTotalDraws() const { return m_TotalDraws; }
private:
    /// 记录一个描述符表根参数（8..31），实际下发推迟到绘制/派发前的 FlushRootArgs
    void SetRootTable(u32 param, D3D12_GPU_DESCRIPTOR_HANDLE handle);
    /// 绘制/派发前把缓存的根参数重新下发。D3D12 的 Set*RootSignature 会清空全部根参数，
    /// 而 Vulkan 切管线时兼容的绑定会保留 —— 引擎按 Vulkan 语义只绑一次，这里负责补齐。
    void FlushRootArgs();
    /// 绘制/派发前，按当前 PSO 反射出的资源用途（SRV 读 / UAV 读写）把已绑定描述符集里的
    /// 纹理与缓冲转到所需状态。引擎按 Vulkan 习惯只发全局内存屏障、常不为「刚写完就读」发
    /// 资源屏障（Intel 的 Vulkan 驱动容忍）；D3D12 下渲染目标未转状态就采样会读到未解压数据，
    /// 缓冲在同一列表里从 UAV 改为 SRV 使用也必须显式转换（GPU 验证 id=942）。
    void EnsureBoundResourceStates();
    /// 把 DEFAULT/CUSTOM 堆上的缓冲转到指定状态（并记入本次录制转换过的缓冲）
    void TransitionBuffer(ID3D12Resource* res, D3D12_RESOURCE_STATES want);

    /// 每个飞行帧槽位一个分配器（同 Vulkan 的逐帧命令缓冲）：分配器在 GPU 执行完之前
    /// 不能 Reset，样例只建一个命令列表并每帧重录，单分配器会在 GPU 仍在执行上一帧时
    /// 被重置 ⇒ GPU 读到被覆盖的命令内存 ⇒ 页错误 / 设备挂起（调试层 id=552）。
    ComPtr<ID3D12CommandAllocator>    m_Allocators[kMaxFramesInFlight];
    ComPtr<ID3D12CommandAllocator>    m_Allocator;          // 当前槽位的分配器
    ComPtr<ID3D12Fence>               m_SlotFence;          // 槽位完成围栏
    u64                               m_SlotFenceValues[kMaxFramesInFlight]{};   // 各槽位最后一次提交的围栏值
    u64                               m_NextFenceValue = 1;
    u32                               m_FrameSlot = 0;
    ComPtr<ID3D12GraphicsCommandList> m_List;
    ComPtr<ID3D12RootSignature>       m_PushRootSig;   // 阶段 0 占位，阶段 2 由 PSO 提供
    D3D12Device*      m_Device = nullptr;
    IRHISwapChain*    m_SwapChain = nullptr;
    D3D12PipelineState* m_Pipeline = nullptr;
    bool m_Recording = false;
    /// 当前管线是否为计算管线（决定绑定哪一套根签名与描述符表）
    bool m_IsComputePipeline = false;
    /// 间接绘制的命令签名缓存。**必须活到 GPU 执行完**：此前每次调用都建局部
    /// ComPtr，函数返回即销毁，调试层报 id=921（签名在列表关闭时被销毁），
    /// 间接绘制因此无效 —— 02.Cube 的实例化网格正是走这条路径。
    std::unordered_map<u32, ComPtr<ID3D12CommandSignature>> m_CmdSigs;
    /// 本帧的推送常量槽位。Vulkan 的 vkCmdPushConstants 语义是「同一块数据、
    /// 多次调用按 offset 累加」，所以整帧必须复用**同一个**槽位；
    /// 每调用一次就新开槽位会让先前的写入丢失（02.Cube 因此黑屏）。
    D3D12_GPU_VIRTUAL_ADDRESS m_PushGpu{};
    void*                     m_PushCpu = nullptr;
    /// 诊断用：记录本帧是否绑过 RTV、以及当前视口
    bool            m_HasRtv = false;
    D3D12_VIEWPORT  m_LastVp{};
    u32             m_PassDraws = 0;
    bool            m_SkipDraws = false;   // 绕行实验：跳过最终 pass 的绘制
    u32             m_TotalDraws = 0;
    u64             m_LastBoundSet = 0;
    u32             m_LastObjIdx = 0;
    bool            m_PushDirty = true;
    /// 根参数缓存：下标即根参数索引（0..7 为 root CBV 不用，8..31 为描述符表）
    D3D12_GPU_DESCRIPTOR_HANDLE m_RootTables[32]{};
    /// 根参数是否需要重新下发（切换管线 / 改了描述符表后置位）
    bool            m_RootArgsDirty = true;
    /// 本次录制中是否已经 SetDescriptorHeaps（描述符表要求先绑堆）
    bool            m_HeapsSet = false;
    /// 各 setIndex 当前绑定的描述符集（EnsureBoundTextureStates 据此找纹理）
    DescriptorSetHandle m_BoundSets[8]{};
    /// 各 setIndex 当前根参数表所指快照对应的集与版本：集换了或内容变了才重新拍快照
    DescriptorSetHandle m_SnapSets[8]{};
    u64                 m_SnapVersions[8]{};
public:
    /// DRED 报告用：按 DRED 记录里事件 / 标记命令的条数，找到对应那次录制记下的名字
    /// （本运行时不为直接写入的 PIX 事件记录面包屑上下文字符串，只能自己对应）。
    /// 找到返回 true，begins / markers 依次是 BeginEvent / SetMarker 的名字
    bool FindEventNames(const ID3D12CommandList* native, u32 beginCount, u32 markerCount,
                        std::vector<std::string>& begins, std::vector<std::string>& markers) const;
    /// 开启 DRED（HE_D3D12_DRED）时为真：才记录事件名，平时零开销
    static bool DredEventLogEnabled();
private:
    /// 每个帧槽位一次录制的事件名记录（与分配器同步轮换：GPU 可能仍在执行的录制都还在）
    struct EventLog { std::vector<std::string> begins, markers; };
    EventLog            m_EventLogs[kMaxFramesInFlight];
    /// 未配对的 BeginDebugLabel 层数：End 时补齐 EndEvent，否则列表关闭失败
    u32                 m_LabelDepth = 0;
    /// SetDrawDebugLabel 记下、等待下一次绘制 / 派发插入的标签
    std::string         m_DrawLabel;
    bool                m_DrawLabelSet = false;
    /// 插入待定的逐绘制标签（FlushRootArgs 开头调用，所有绘制 / 派发路径都经过它）
    void EmitDrawLabel();
    /// 本次录制用过的快照环形区块（位掩码），提交时交给设备标记围栏
    u32                 m_SetRingChunkMask = 0;
    /// 绘制/派发前调用：为已绑定的描述符集准备快照，并更新 SRV / UAV 表根参数
    void PrepareSetTables();
    /// 当前 pass 正作为附件的资源：它们不能同时被转成可读状态
    std::vector<ID3D12Resource*> m_PassAttachments;
    /// 当前 pass 绑定的 RTV / DSV：切换只读 DSV 时需要重新 OMSetRenderTargets
    D3D12_CPU_DESCRIPTOR_HANDLE  m_PassRtvs[8]{};
    u32                          m_PassRtvCount = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE  m_PassDsv{};
    ID3D12Resource*              m_PassDsvRes = nullptr;
    bool                         m_DsvReadOnly = false;   // 当前是否绑着只读 DSV
    /// 记录本 pass 的附件（RTV 数组 + DSV），供状态转换跳过与只读 DSV 切换使用
    void SetPassAttachments(const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, u32 rtvCount, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv);
    void ClearPassAttachments();
    std::vector<D3D12_RESOURCE_BARRIER> m_BarrierScratch;   // 复用的临时数组
    /// 缓冲的状态按命令列表各自跟踪：缓冲在每次 ExecuteCommandLists 执行完后都会衰减回 COMMON，
    /// 所以每个列表开始录制时所有缓冲都是 COMMON（Begin 时清空）。用设备级的全局记录会被
    /// 其他命令列表的转换干扰，屏障的 StateBefore 与实际不符（GPU 验证 id=942）。
    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> m_BufferStates;
    /// 录制期记下的时间线等待 / 信号：与 Vulkan 一样附在**这次提交**上，
    /// 等待在 ExecuteCommandLists 之前、信号在之后下到队列
    std::vector<std::pair<RHIFenceHandle, u64>> m_PendingWaits;
    std::vector<std::pair<RHIFenceHandle, u64>> m_PendingSignals;
    /// 各帧槽位录制期间创建的临时资源（如纹理回读的中转缓冲）：该槽位下次 Begin 等到围栏后释放
    std::vector<ComPtr<ID3D12Resource>> m_TempResources[kMaxFramesInFlight];
    /// 本次录制写过的查询：提交时（OnSubmitted）打上围栏值，之后才算可读
    std::vector<std::pair<class D3D12QueryPool*, u32>> m_PendingQueries;
    /// 读取查询结果（wait 为真时阻塞等待），返回是否全部可用
    bool ReadQueryResults(IRHIQueryPool* pool, u32 first, u32 count, u64* data, bool wait);
    D3D12_RESOURCE_STATES GetBufferState(ID3D12Resource* res) const {
        auto it = m_BufferStates.find(res);
        return it != m_BufferStates.end() ? it->second : D3D12_RESOURCE_STATE_COMMON;
    }
    u32             m_LastBaseSlot = 0;      // 本列表累计绘制数（诊断用）
    bool            m_PassHasRtv = false;
    u32             m_PassW = 0;
    u32             m_PassH = 0;
};

} // namespace he::rhi::d3d12

#endif // _WIN32
