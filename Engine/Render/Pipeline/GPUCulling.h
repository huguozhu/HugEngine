#pragma once

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
    static constexpr u32 kMaxObjects = 2048;

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
    rhi::IRHIBuffer* GetIndirectBuffer() const { return m_IndirectCmdBuf.get(); }
    rhi::IRHIBuffer* GetDrawCountBuffer() const { return m_DrawCountBuf.get(); }
    rhi::DescriptorSetHandle GetPhase2Set() const { return m_Phase2Set; }
    rhi::IRHISampler* GetHiZSampler() const { return m_HiZSampler.get(); }

private:
    bool m_Initialized = false;
    rhi::IRHIDevice* m_Device = nullptr;  // 缓存设备指针（描述符更新用）

    // 单阶段/Phase 1 PSO
    rhi::ShaderBytecode m_CS;
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_DescLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_DescSet    = rhi::kInvalidSet;

    // GPU 缓冲（单阶段 & Phase 2 共用）
    std::unique_ptr<rhi::IRHIBuffer> m_IndirectCmdBuf;     // IndirectDrawCommand[]
    std::unique_ptr<rhi::IRHIBuffer> m_DrawCountBuf;

    // Phase 1 专用资源
    std::unique_ptr<rhi::IRHIBuffer> m_CandidateBuf;       // Phase 1 → Phase 2 候选列表（u32[]）
    std::unique_ptr<rhi::IRHIBuffer> m_CandidateCountBuf;  // 候选数量

    // Phase 1 PSO（裁剪后输出候选索引）
    rhi::ShaderBytecode m_Phase1CS;
    std::unique_ptr<rhi::IRHIPipelineState> m_Phase1PSO;
    rhi::DescriptorSetLayoutHandle m_Phase1Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Phase1Set    = rhi::kInvalidSet;

    // Phase 2 PSO（精筛候选 → IndirectDraw）
    rhi::ShaderBytecode m_Phase2CS;
    std::unique_ptr<rhi::IRHIPipelineState> m_Phase2PSO;
    rhi::DescriptorSetLayoutHandle m_Phase2Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Phase2Set    = rhi::kInvalidSet;

    // Hi-Z 金字塔
    static constexpr u32 kHiZMips = 8;  // 最多 8 级（最大支持 256×256 以上的分辨率）
    std::unique_ptr<rhi::IRHITexture> m_HiZTexture;      // 完整 Hi-Z 深度金字塔（ShaderResource | UnorderedAccess）
    std::unique_ptr<rhi::IRHISampler> m_HiZSampler;      // 点采样器
    std::unique_ptr<rhi::IRHIPipelineState> m_HiZ_PSO;   // HiZDownsample PSO
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
    std::unique_ptr<rhi::IRHIPipelineState> m_PTG_PSO;
    std::unique_ptr<rhi::IRHIBuffer> m_PTGParamBuf;       // 每帧参数 + 帧触发信号
    rhi::DescriptorSetLayoutHandle m_PTGLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_PTGSet    = rhi::kInvalidSet;
    bool m_PTGActive = false;
    u32 m_PTGFrameCounter = 1;  // PTG 帧计数器（从 1 开始，0 为初始空闲态）

    float4x4 m_LastViewProj = float4x4(1.0f);  // 最近一次设置的 ViewProj（Phase 2 复用）
    u32 m_LastVisibleCount = 0;
};

} // namespace he::render
