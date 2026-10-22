#pragma once

#include "Threading/RenderThreadContext.h"   // T2.6：帧上下文（唯一 RHI 出口）

#include "Pipeline/Material.h"  // kMaxGPUObjects
#include "RHI/RHI.h"
#include "RHI/Shader.h"
#include "Math/Math.h"
#include "Math/Geometry.h"
#include <vector>
#include <memory>

// ============================================================
// GPUCulling — Compute Shader 视锥+遮挡剔除
//
// 单阶段模式（useTwoPhase=false，默认）:
//   每帧 Dispatch() 直接输出 IndirectDraw 命令
//
// 两阶段模式（useTwoPhase=true）:
//   Phase 1: 视锥剔除 + 上帧 Hi-Z 粗筛 → 候选列表（AsyncCompute 队列，帧首）
//   Phase 2: 当前帧 Hi-Z 精筛 → IndirectDraw 命令（GBuffer 之后）
// ============================================================

namespace he::render {

// 每个物体的 AABB + GPUObjectData 索引
struct alignas(16) CullObjectBounds {
    float4 minPoint;    // AABB min（世界空间）
    float4 maxPoint;    // AABB max
    u32    objectIndex; // GPUObjectData[] 中的索引
    u32    _pad[3];
};

class GPUCulling {
public:
    static constexpr u32 kMaxObjects = kMaxGPUObjects;  // 统一到 Material.h

    bool enabled = true;

    /// 启用两阶段剔除（默认关闭，向后兼容）
    bool useTwoPhase = false;

    /// 持久化线程组模式（默认关闭，与两阶段剔除互斥）
    bool usePTG = true;
    static constexpr u32 kPTGGroupCount = 1;  // 单线程组：64 线程并行剔除，避免跨组同步

    /// 初始化 PTG（一次性 Dispatch，之后通过 Signal 触发处理）
    bool InitializePTG(rhi::IRHIDevice* device);

    /// 每帧触发 PTG 处理（写入视锥/Hi-Z 参数 + 帧信号）
    void SignalPTG(rhi::IRHICommandList* cmd, const float4x4& viewProj,
                   u32 objectCount, u32 screenW, u32 screenH);

    /// 关闭 PTG 线程（发送 0xFFFFFFFF 退出信号）
    void ShutdownPTG(rhi::IRHIDevice* device);

    /// 初始化：创建 SSBO + Compute PSO
    bool Initialize(rhi::IRHIDevice* device);
    /// 清理 GPU 资源
    void Shutdown(rhi::IRHIDevice* device);

    /// 设置 GPUScene SSBO + 深度缓冲输入
    void SetSceneBuffer(rhi::IRHIDevice* device, rhi::IRHIBuffer* gpuSceneSSBO);
    void SetDepthTexture(rhi::IRHIDevice* device, rhi::IRHITexture* depthTex, u32 width, u32 height);

    /// 单阶段剔除（默认模式，useTwoPhase=false 时使用）
    void Dispatch(rhi::IRHICommandList* cmd, const float4x4& viewProj, u32 objectCount, u32 screenW, u32 screenH);

    // ============================================================
    // 两阶段剔除 API（useTwoPhase=true 时使用）
    // ============================================================

    /// Phase 1: 粗筛（帧首，AsyncCompute 队列）
    /// 使用上帧深度做视锥+Hi-Z 剔除，输出候选物体索引
    void DispatchPhase1(rhi::IRHICommandList* cmd, const float4x4& viewProj,
                        u32 objectCount, u32 screenW, u32 screenH);

    /// 构建当前帧 Hi-Z 深度金字塔（GBuffer 后、Phase 2 前调用）
    void BuildHiZPyramid(rhi::IRHICommandList* cmd, u32 screenW, u32 screenH);

    /// Phase 2: 精筛（GBuffer 后）
    /// 使用当前帧 Hi-Z 金字塔验证 Phase 1 候选
    void DispatchPhase2(rhi::IRHICommandList* cmd, u32 screenW, u32 screenH);

    /// 获取 Phase 1 候选 buffer 的尺寸（u32 数量）
    u32 GetPhase1CandidateCount() const;

    /// 读回可见物体索引列表（CPU 同步）
    void Readback(rhi::IRHIDevice* device, std::vector<u32>& outVisibleIndices);

    u32 GetLastVisibleCount() const { return m_LastVisibleCount; }
    /// A-3：句柄按需解析（代次过期 ⇒ nullptr）
    rhi::IRHIBuffer* GetIndirectBuffer() const {
        return m_Device ? m_Device->ResolveBuffer(m_IndirectCmdBuf) : nullptr;
    }
    rhi::IRHIBuffer* GetDrawCountBuffer() const {
        return m_Device ? m_Device->ResolveBuffer(m_DrawCountBuf) : nullptr;
    }
    rhi::DescriptorSetHandle GetPhase2Set() const { return m_Phase2Set; }
    // Phase 2 描述符集绑定号（供外部引用——如 DeferredPipeline_FrameGraph 每帧更新 Hi-Z 绑定）
    static constexpr u32 kPhase2BindHiZ = 3;
    rhi::IRHISampler* GetHiZSampler() const { return m_Device ? m_Device->ResolveSampler(m_HiZSampler) : nullptr; }
    /// Hi-Z 深度金字塔纹理（供 SSR 层次追踪复用）
    rhi::IRHITexture* GetHiZTexture() const { return m_Device ? m_Device->ResolveTexture(m_HiZTexture) : nullptr; }

private:
    bool m_Initialized = false;
    rhi::IRHIDevice* m_Device = nullptr;  // 缓存设备指针（描述符更新用）

    /// 【T2.6】本帧的渲染线程上下文（可空；空时解析回退 `m_Device`）
    const RenderThreadContext* m_FrameCtx = nullptr;

public:
    void SetFrameContext(const RenderThreadContext* ctx) { m_FrameCtx = ctx; }

    [[nodiscard]] rhi::IRHIBuffer* ResB(rhi::RHIBufferHandle h) const {
        const RenderThreadContext* c = m_FrameCtx ? m_FrameCtx : GetFrameContext();
        if (c) return c->ResolveBuffer(h);
        return m_Device ? m_Device->ResolveBuffer(h) : nullptr;
    }
    [[nodiscard]] rhi::IRHITexture* ResT(rhi::RHITextureHandle h) const {
        const RenderThreadContext* c = m_FrameCtx ? m_FrameCtx : GetFrameContext();
        if (c) return c->ResolveTexture(h);
        return m_Device ? m_Device->ResolveTexture(h) : nullptr;
    }
    [[nodiscard]] rhi::IRHIPipelineState* ResP(rhi::RHIPipelineStateHandle h) const {
        const RenderThreadContext* c = m_FrameCtx ? m_FrameCtx : GetFrameContext();
        if (c) return c->ResolvePipelineState(h);
        return m_Device ? m_Device->ResolvePipelineState(h) : nullptr;
    }
    [[nodiscard]] rhi::IRHISampler* ResS(rhi::RHISamplerHandle h) const {
        const RenderThreadContext* c = m_FrameCtx ? m_FrameCtx : GetFrameContext();
        if (c) return c->ResolveSampler(h);
        return m_Device ? m_Device->ResolveSampler(h) : nullptr;
    }


    // 单阶段/Phase 1 PSO
    rhi::ShaderBytecode m_CS;
    rhi::RHIPipelineStateHandle m_PSO{};   // A-3：句柄持有
    rhi::DescriptorSetLayoutHandle m_DescLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_DescSet    = rhi::kInvalidSet;

    // GPU 缓冲（单阶段 & Phase 2 共用）
    rhi::RHIBufferHandle m_IndirectCmdBuf{};     // A-3：IndirectDrawCommand[]
    rhi::RHIBufferHandle m_DrawCountBuf{};

    // Phase 1 专用资源
    rhi::RHIBufferHandle m_CandidateBuf{};       // A-3：Phase 1 → Phase 2 候选列表（u32[]）
    rhi::RHIBufferHandle m_CandidateCountBuf{};  // A-3：候选数量

    // Phase 1 PSO（裁剪后输出候选索引）
    rhi::ShaderBytecode m_Phase1CS;
    rhi::RHIPipelineStateHandle m_Phase1PSO{};   // A-3：句柄持有
    rhi::DescriptorSetLayoutHandle m_Phase1Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Phase1Set    = rhi::kInvalidSet;

    // Phase 2 PSO（精筛候选 → IndirectDraw）
    rhi::ShaderBytecode m_Phase2CS;
    rhi::RHIPipelineStateHandle m_Phase2PSO{};   // A-3：句柄持有
    rhi::DescriptorSetLayoutHandle m_Phase2Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Phase2Set    = rhi::kInvalidSet;

    // Hi-Z 金字塔
    static constexpr u32 kHiZMips = 8;  // 最多 8 级（最大支持 256×256 以上的分辨率）
    rhi::RHITextureHandle m_HiZTexture{};      // A-3：完整 Hi-Z 深度金字塔（ShaderResource | UnorderedAccess）
    rhi::RHISamplerHandle m_HiZSampler{};      // A-3：点采样器
    rhi::RHIPipelineStateHandle m_HiZ_PSO{};   // A-3：句柄持有   // HiZDownsample PSO
    rhi::DescriptorSetLayoutHandle m_HiZLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_HiZSet    = rhi::kInvalidSet;  // 单 set，每次 dispatch 前更新
    u32 m_HiZMipCount = 0;                 // 金字塔层数（0=禁用, 1=仅全分辨率, kHiZMips=完整）
    rhi::IRHITexture* m_DepthInput = nullptr;  // 当前帧深度缓冲输入

    // 每 mip level 的图像视图（用于 Hi-Z 金字塔构建）
    struct HiZMipViews {
        void* storageView = nullptr;  // RWTexture2D 写入用
        void* sampledView = nullptr;  // Texture2D 读取用
    };
    HiZMipViews m_MipViews[kHiZMips];

    // PTG 资源
    rhi::ShaderBytecode m_PTGCS;
    rhi::RHIPipelineStateHandle m_PTG_PSO{};   // A-3：句柄持有
    rhi::RHIBufferHandle m_PTGParamBuf{};   // A-3：每帧参数 + 帧触发信号
    rhi::DescriptorSetLayoutHandle m_PTGLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_PTGSet    = rhi::kInvalidSet;
    bool m_PTGActive = false;
    u32 m_PTGFrameCounter = 1;  // PTG 帧计数器（从 1 开始，0 为初始空闲态）

    float4x4 m_LastViewProj = float4x4(1.0f);  // 最近一次设置的 ViewProj（Phase 2 复用）
    u32 m_LastVisibleCount = 0;
    u32 m_FrameIndex        = 0;  // 首帧跳过 Hi-Z，等上帧深度有效
};

} // namespace he::render
