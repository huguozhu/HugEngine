// ============================================================================
//  D3D11CommandList.h —— D3D11 命令列表
//
//  设计要点（D3D11 后端实现规划 3.4）：
//    · 主录制走 immediate context：命令在录制时就已发给驱动，Submit 只负责围栏信号；
//    · 多个命令列表共用同一个 immediate context（如 AI 模块在主列表录制中途另开列表派发）：
//      每个列表保存自己完整的绑定状态，设备记录「上下文当前装着谁的状态」。任何改动上下文的
//      调用前先 EnsureOwner()：上下文被别的列表动过，就先把本列表的完整状态重新应用一遍
//      （M2.6），保证上下文里永远不会是两个列表状态的混合；
//    · 描述符集不在 BindDescriptorSet 时下发：绘制 / 派发前按**当前 PSO** 的绑定槽位
//      从已绑定的集里取资源、现建视图并绑到 D3D11 槽位（FlushBindings，M2.5）；
//    · 屏障接口全部为空操作（D3D11 驱动自动跟踪资源冲突），但要主动解绑 SRV / RTV 冲突（M3.4）；
//    · 光追、mesh shader、队列归属等 D3D11 没有的能力为空操作。
// ============================================================================
#pragma once

#if defined(_WIN32)

#include "D3D11Common.h"
#include <string>
#include <utility>
#include <vector>

namespace he::rhi::d3d11 {

class D3D11Device;
class D3D11PipelineState;
class D3D11Texture;

class D3D11CommandList final : public IRHICommandList {
public:
    explicit D3D11CommandList(D3D11Device* device) : m_Device(device) {}
    ~D3D11CommandList() override;

    // ---- 生命周期 ----
    void Begin() override;
    void End()   override;
    /// 二级列表（deferred context）放到 M7，当前设备不创建二级列表
    void BeginSecondary(IRHIPipelineState*) override {}
    void ExecuteSecondary(IRHICommandList*) override {}
    bool IsSecondary() const override { return false; }

    // ---- Render pass：设置渲染目标并按加载操作清除；D3D11 没有 render pass 对象 ----
    void BeginRenderPass(u32 colorCount, Format colorFormat, Format depthFormat = Format::Unknown,
                         const ClearValue* clear = nullptr, LoadOp loadOp = LoadOp::Clear) override;
    void EndRenderPass() override;
    void BeginOffscreenPass(void* colorImageView, void* depthImageView, u32 width, u32 height,
                            const ClearValue* clear = nullptr, bool allowSecondary = false) override;
    void BeginOffscreenPassMRT(void* const* colorImageViews, u32 colorCount, void* depthImageView,
                               u32 width, u32 height, const ClearValue* clears = nullptr,
                               bool allowSecondary = false) override;
    void EndOffscreenPass() override;
    void SetSwapChain(IRHISwapChain* sc) override { m_SwapChain = sc; }

    // ---- 管线与资源绑定 ----
    void SetPipeline(IRHIPipelineState* pso) override;
    void SetVertexBuffer(IRHIBuffer* buffer, u32 binding = 0) override;
    void SetIndexBuffer(IRHIBuffer* buffer, u32 offset = 0) override;
    void SetViewport(const Viewport& vp) override;
    void SetScissor(const ScissorRect& sc) override;
    /// D3D 的视口高度不能为负、NDC 的 y 向上：用正高度覆盖整张目标（不翻转，与 D3D12 一致）
    void SetFullscreenViewport(u32 width, u32 height) override;
    /// 只记录：真正的绑定在绘制 / 派发前按当前 PSO 下发（FlushBindings）
    void BindDescriptorSet(u32 setIndex, DescriptorSetHandle set) override;
    /// 记录 bindless 替代绑定 → bindless 堆句柄（绘制 / 派发前由 FlushBindings 解析成资源）
    void SetNonBindlessHandle(u32 binding, u32 handle) override;
    /// Vulkan 语义：同一块 256 字节数据按 offset 累加写入，直到被覆盖前一直有效。
    /// 只写 CPU 副本；绘制 / 派发前若有改动（或常量环换了内存）才写进常量环新槽（规划 3.5）
    void SetPushConstants(u32 offset, u32 size, const void* data) override;

    // ---- 绘制 ----
    void Draw(u32 vertexCount, u32 instanceCount = 1, u32 firstVertex = 0, u32 firstInstance = 0) override;
    void DrawIndexed(u32 indexCount, u32 instanceCount = 1, u32 firstIndex = 0,
                     i32 vertexOffset = 0, u32 firstInstance = 0) override;
    /// D3D11 一次只能画一条间接命令：按条数逐条调用 DrawIndexedInstancedIndirect（规划 3.3）
    void DrawIndexedIndirect(IRHIBuffer* buffer, u64 offset, u32 drawCount, u32 stride) override;
    /// 内置 CS 把序号 ≥ GPU 计数的命令 instanceCount 置 0，再逐条绘制（条数按异步回读的计数估计，规划 3.3）
    void DrawIndexedIndirectCount(IRHIBuffer* buffer, u64 offset, IRHIBuffer* countBuffer, u64 countOffset,
                                  u32 maxDrawCount, u32 stride) override;
    // D3D11 没有 mesh shader（supportsMeshShaders=false，引擎不会走这条路径）
    void DrawMeshTasks(u32, u32 = 1, u32 = 1) override {}
    void DrawMeshTasksIndirect(IRHIBuffer*, u64, u32, u32) override {}
    void DrawMeshTasksIndirectCount(IRHIBuffer*, u64, IRHIBuffer*, u64, u32, u32) override {}

    // ---- 计算 ----
    void Dispatch(u32 x, u32 y, u32 z) override;
    void DispatchIndirect(IRHIBuffer* buffer, u64 offset) override;

    // ---- 同步：D3D11 驱动自动跟踪资源冲突，屏障为空操作 ----
    void PipelineBarrier(PipelineStage, PipelineStage, ResourceState, ResourceState) override {}
    void PipelineBarrier(PipelineStage, PipelineStage, ResourceState, ResourceState, IRHITexture*) override {}

    // ---- 拷贝与清除 ----
    void CopyBuffer(IRHIBuffer* src, IRHIBuffer* dst, u64 size, u64 srcOffset = 0, u64 dstOffset = 0) override;
    void CopyTextureToTexture(IRHITexture* src, IRHITexture* dst) override;
    /// 纹理 (mip 0, 层 0) 的矩形区域 → 缓冲（从 bufferOffset 起逐行紧密排列）。
    /// D3D11 不能把纹理拷进缓冲：经暂存纹理同步读回，再写进缓冲
    void CopyTextureToBuffer(IRHITexture* src, IRHIBuffer* dst, u32 x = 0, u32 y = 0,
                             u32 width = 0, u32 height = 0, u64 bufferOffset = 0) override;
    void ClearDepthStencil(IRHITexture* texture, float depth = 1.0f) override;

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

    // ---- 时间线与查询 ----
    /// 记录本次提交要发的时间线信号：Submit 时在 immediate context 上 Signal（命令都已发出，信号排在其后）
    void SetTimelineSignal(RHIFenceHandle fence, u64 value) override;
    /// 只有一个 immediate context：等待的信号必然来自更早提交的命令，已经满足，忽略
    void SetTimelineWait(RHIFenceHandle, u64) override {}
    // 查询（M3.7）：D3D11 的查询对象直接在 immediate context 上 Begin / End，见 D3D11QueryPool
    void WriteTimestamp(IRHIQueryPool* pool, u32 queryIndex) override;
    void ResetQueryPool(IRHIQueryPool* pool) override;
    void GetQueryResults(IRHIQueryPool* pool, u32 first, u32 count, u64* data) override;
    bool TryGetQueryResults(IRHIQueryPool* pool, u32 first, u32 count, u64* data) override;
    void BeginQuery(IRHIQueryPool* pool, u32 queryIndex) override;
    void EndQuery(IRHIQueryPool* pool, u32 queryIndex) override;

    // ---- 调试标签（ID3DUserDefinedAnnotation：RenderDoc / PIX 显示为分组与标记）----
    void BeginDebugLabel(const char* name, const float color[4] = nullptr) override;
    void EndDebugLabel() override;
    void InsertDebugLabel(const char* name, const float color[4] = nullptr) override;

    /// 下发录制期记下的时间线信号（由 D3D11Device::Submit 调用）
    void Submit() override;

    /// 供外部渲染器（ImGui 的 D3D11 后端）直接往上下文里画：先确保上下文装的是本列表的状态
    /// （渲染目标、视口等），返回 immediate context
    ID3D11DeviceContext4* PrepareExternalRendering();

private:
    /// 当前 PSO 用到推送常量时：数据有改动或常量环换过内存就写进新槽，再绑定到 b0
    void BindPushConstants();
    /// 绘制 / 派发前调用：当前 PSO 可用时，确保上下文装的是本列表的状态、下发渲染目标 / 顶点缓冲 /
    /// 描述符集并返回 true；PSO 不可用（着色器没有 DXBC）时返回 false，调用方跳过这次绘制 / 派发
    bool PrepareDrawOrDispatch(bool compute);
    /// 按当前 PSO 的绑定槽位，把已绑定描述符集里的资源绑到 D3D11 槽位
    void FlushBindings();
    /// 冲突解绑（规划 3.7）：这些 UAV 的资源若仍以 SRV 绑着，先清掉那些 SRV 槽
    void UnbindSrvsOfUavs(ID3D11UnorderedAccessView* const* uavs, u32 count);
    /// 一次绑定缺资源时打印（同一 PSO + binding 只打一次，避免每帧刷屏）
    void ReportMissing(u32 set, u32 binding, const char* what);
    /// bindless 替代绑定（kBindingNB*）：按 SetNonBindlessHandle 记下的句柄从 bindless 堆取资源。
    /// 返回 true 表示该 binding 由句柄决定（取不到时输出为空）；false 表示走描述符集
    bool ResolveNonBindless(u32 binding, u32 element, ID3D11ShaderResourceView*& srv,
                            ID3D11SamplerState*& smp, class D3D11Buffer*& buf) const;

    /// 上下文被别的列表动过时，把本列表的完整状态重新应用，并把上下文标为本列表所有
    void EnsureOwner();
    /// 应用当前 PSO 的着色器与固定功能状态
    void ApplyPipeline();
    /// 应用渲染目标（深度按当前 PSO 是否写深度选普通 / 只读 DSV）
    void ApplyRenderTargets();
    /// 应用顶点 / 索引缓冲（步长取当前 PSO 的顶点步长）以及零缓冲、实例序号缓冲
    void ApplyInputAssembler();
    void ApplyViewportScissor();
    /// 开始一个渲染通道：记录目标、按需清除、视口与裁剪矩形设为整个目标
    void BeginPassTargets(ID3D11RenderTargetView* const* rtvs, u32 rtvCount, void* depthView,
                          u32 width, u32 height, bool clearColor, const ClearValue* colorClears,
                          bool clearDepth, float depth, u8 stencil);
    /// 引擎的视图句柄 → RTV（纹理视图登记表；交换链后缓冲由 BeginRenderPass 单独处理）
    ID3D11RenderTargetView* ResolveRtv(void* view) const;

    D3D11Device*        m_Device    = nullptr;
    IRHISwapChain*      m_SwapChain = nullptr;

    // ---- 本列表的完整绑定状态（EnsureOwner 据此重新应用）----
    D3D11PipelineState* m_Pipeline  = nullptr;
    static constexpr u32 kMaxSets = 8;
    DescriptorSetHandle m_Sets[kMaxSets]{};
    static constexpr u32 kMaxVertexSlots = 14;   // 14 / 15 两个槽给实例序号缓冲与零缓冲
    IRHIBuffer*         m_VertexBuffers[kMaxVertexSlots]{};
    IRHIBuffer*         m_IndexBuffer = nullptr;
    u32                 m_IndexOffset = 0;
    ID3D11RenderTargetView* m_Rtvs[kMaxColorAttachments]{};
    u32                 m_RtvCount = 0;
    D3D11Texture*       m_DepthTexture = nullptr;   // 当前通道的深度附件（只读 / 可写 DSV 由 PSO 决定）
    u32                 m_DepthMip = 0, m_DepthLayer = 0;
    bool                m_DepthReadOnly = false;    // 当前已应用的是否只读 DSV
    D3D11_VIEWPORT      m_Viewport{};
    D3D11_RECT          m_Scissor{};

    // ---- 推送常量 ----
    u8                  m_PushData[256]{};          // CPU 副本（Vulkan 推送常量的完整 256 字节）
    bool                m_PushDirty = true;         // 自上次写入常量环后有改动
    u32                 m_PushFirstConstant = 0;    // 当前数据所在的常量环槽（以 16 字节常量计）
    u64                 m_PushGeneration = 0;       // 该槽所属的常量环代次（0 = 还没写过）

    std::vector<std::pair<RHIFenceHandle, u64>> m_PendingSignals;   // 本次提交要发的时间线信号
    std::vector<std::pair<u32, u32>> m_NonBindless;   // bindless 替代绑定 → bindless 堆句柄（条目很少，线性查找）

    u32                 m_LabelDepth = 0;           // 未配对的 BeginDebugLabel 层数（End 时补齐）
    std::string         m_LastMissing;              // 上一次报告缺资源的键（同一键不重复打印）
    bool                m_WarnedIndirectCount = false;
    bool                m_WarnedDispatchClamp = false;   // 派发工作组数超上限的提示只打一次
    bool                m_WarnedUavAlias = false;        // 同一资源绑到多个 UAV 槽的提示只打一次
};

} // namespace he::rhi::d3d11

#endif // _WIN32
