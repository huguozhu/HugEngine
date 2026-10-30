// ============================================================================
//  D3D11CommandList.h —— D3D11 命令列表
//
//  设计要点（D3D11 后端实现规划 3.4）：
//    · 主录制走 immediate context：命令在录制时就已发给驱动，Submit 只负责围栏信号；
//    · 多个命令列表共用同一个 immediate context，每个列表保存自己的绑定状态，
//      换列表后的第一次绘制 / 派发前整体重新应用（M2.6）；
//    · 描述符集不在 BindDescriptorSet 时下发：绘制 / 派发前按**当前 PSO** 的绑定槽位
//      从已绑定的集里取资源、现建视图并绑到 D3D11 槽位（FlushBindings，M2.5）；
//    · 屏障接口全部为空操作（D3D11 驱动自动跟踪资源冲突），但要主动解绑 SRV / RTV 冲突（M3.4）；
//    · 光追、mesh shader、队列归属等 D3D11 没有的能力为空操作。
// ============================================================================
#pragma once

#if defined(_WIN32)

#include "D3D11Common.h"
#include <string>

namespace he::rhi::d3d11 {

class D3D11Device;
class D3D11PipelineState;

class D3D11CommandList final : public IRHICommandList {
public:
    explicit D3D11CommandList(D3D11Device* device) : m_Device(device) {}

    // ---- 生命周期 ----
    void Begin() override;
    void End()   override {}
    /// 二级列表（deferred context）放到 M7，当前设备不创建二级列表
    void BeginSecondary(IRHIPipelineState*) override {}
    void ExecuteSecondary(IRHICommandList*) override {}
    bool IsSecondary() const override { return false; }

    // ---- Render pass（M2.6）----
    void BeginRenderPass(u32, Format, Format = Format::Unknown,
                         const ClearValue* = nullptr, LoadOp = LoadOp::Clear) override {}
    void EndRenderPass() override {}
    void BeginOffscreenPass(void*, void*, u32, u32, const ClearValue* = nullptr, bool = false) override {}
    void BeginOffscreenPassMRT(void* const*, u32, void*, u32, u32,
                               const ClearValue* = nullptr, bool = false) override {}
    void EndOffscreenPass() override {}
    void SetSwapChain(IRHISwapChain* sc) override { m_SwapChain = sc; }

    // ---- 管线与资源绑定 ----
    void SetPipeline(IRHIPipelineState* pso) override;
    void SetVertexBuffer(IRHIBuffer*, u32 = 0) override {}
    void SetIndexBuffer(IRHIBuffer*, u32 = 0) override {}
    void SetViewport(const Viewport&) override {}
    void SetScissor(const ScissorRect&) override {}
    void SetFullscreenViewport(u32, u32) override {}
    /// 只记录：真正的绑定在绘制 / 派发前按当前 PSO 下发（FlushBindings）
    void BindDescriptorSet(u32 setIndex, DescriptorSetHandle set) override;
    void SetPushConstants(u32, u32, const void*) override {}

    // ---- 绘制 ----
    void Draw(u32 vertexCount, u32 instanceCount = 1, u32 firstVertex = 0, u32 firstInstance = 0) override;
    void DrawIndexed(u32 indexCount, u32 instanceCount = 1, u32 firstIndex = 0,
                     i32 vertexOffset = 0, u32 firstInstance = 0) override;
    void DrawIndexedIndirect(IRHIBuffer*, u64, u32, u32) override {}
    void DrawIndexedIndirectCount(IRHIBuffer*, u64, IRHIBuffer*, u64, u32, u32) override {}
    // D3D11 没有 mesh shader（supportsMeshShaders=false，引擎不会走这条路径）
    void DrawMeshTasks(u32, u32 = 1, u32 = 1) override {}
    void DrawMeshTasksIndirect(IRHIBuffer*, u64, u32, u32) override {}
    void DrawMeshTasksIndirectCount(IRHIBuffer*, u64, IRHIBuffer*, u64, u32, u32) override {}

    // ---- 计算 ----
    void Dispatch(u32 x, u32 y, u32 z) override;
    void DispatchIndirect(IRHIBuffer*, u64) override {}

    // ---- 同步：D3D11 驱动自动跟踪资源冲突，屏障为空操作 ----
    void PipelineBarrier(PipelineStage, PipelineStage, ResourceState, ResourceState) override {}
    void PipelineBarrier(PipelineStage, PipelineStage, ResourceState, ResourceState, IRHITexture*) override {}

    // ---- 拷贝（M2.6）----
    void CopyBuffer(IRHIBuffer*, IRHIBuffer*, u64, u64 = 0, u64 = 0) override {}
    void CopyTextureToTexture(IRHITexture*, IRHITexture*) override {}

    // ---- 光追：D3D11 不支持（supportsRayTracing=false）----
    void BuildBLAS(IRHIAccelerationStructure*, IRHIBuffer*, const BLASBuildDesc&, bool = false) override {}
    void BuildTLAS(IRHIAccelerationStructure*, IRHIBuffer*, IRHIBuffer*, u32, bool = false) override {}
    void BindRTPipeline(IRHIRayTracingPipelineState*) override {}
    void TraceRays(const SBTDesc&, u32, u32, u32 = 1) override {}

    // ---- 队列归属：D3D11 只有一个上下文，无对应概念 ----
    void QueueOwnershipTransfer(IRHITexture*, QueueType, QueueType, ResourceState, ResourceState) override {}
    void QueueOwnershipTransfer(IRHIBuffer*, QueueType, QueueType, ResourceState, ResourceState) override {}
    void ReleaseToQueue(IRHITexture*, QueueType) override {}
    void AcquireFromQueue(IRHITexture*, QueueType) override {}

    // ---- 时间线与查询（M2.7 / M3.7）----
    void SetTimelineSignal(RHIFenceHandle, u64) override {}
    void SetTimelineWait(RHIFenceHandle, u64) override {}
    void WriteTimestamp(IRHIQueryPool*, u32) override {}
    void ResetQueryPool(IRHIQueryPool*) override {}
    void GetQueryResults(IRHIQueryPool*, u32, u32 count, u64* data) override {
        // 未实现前返回 0，调用方读到的计时为 0 而不是未初始化内存
        if (data) for (u32 i = 0; i < count; ++i) data[i] = 0;
    }
    bool TryGetQueryResults(IRHIQueryPool*, u32, u32, u64*) override { return false; }

    void Submit() override {}

private:
    /// 绘制 / 派发前调用：当前 PSO 可用时，把已绑定描述符集里的资源绑到 D3D11 槽位并返回 true；
    /// PSO 不可用（着色器没有 DXBC）时返回 false，调用方跳过这次绘制 / 派发
    bool PrepareDrawOrDispatch(bool compute);
    /// 按当前 PSO 的绑定槽位，把已绑定描述符集里的资源绑到 D3D11 槽位
    void FlushBindings();
    /// 一次绑定缺资源时打印（同一 PSO + binding 只打一次，避免每帧刷屏）
    void ReportMissing(u32 set, u32 binding, const char* what);

    D3D11Device*        m_Device    = nullptr;
    IRHISwapChain*      m_SwapChain = nullptr;
    D3D11PipelineState* m_Pipeline  = nullptr;
    static constexpr u32 kMaxSets = 8;
    DescriptorSetHandle m_Sets[kMaxSets]{};
    std::string         m_LastMissing;   // 上一次报告缺资源的键（同一键不重复打印）
};

} // namespace he::rhi::d3d11

#endif // _WIN32
