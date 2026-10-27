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
    void WriteTimestamp(IRHIQueryPool*, u32) override {}
    void ResetQueryPool(IRHIQueryPool*) override {}
    void GetQueryResults(IRHIQueryPool*, u32, u32, u64*) override {}
    bool TryGetQueryResults(IRHIQueryPool*, u32, u32, u64*) override { return false; }

    void Submit() override;

    /// 供 Device::Submit 使用
    ID3D12GraphicsCommandList* GetNativeCommandList() const { return m_List.Get(); }

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
    ComPtr<ID3D12CommandAllocator>    m_Allocator;
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
    u32             m_LastBaseSlot = 0;      // 本列表累计绘制数（诊断用）
    bool            m_PassHasRtv = false;
    u32             m_PassW = 0;
    u32             m_PassH = 0;
};

} // namespace he::rhi::d3d12

#endif // _WIN32
