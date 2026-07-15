#pragma once

#include "RHI/RHI.h"
#include "Pipeline/Material.h"
#include "Pipeline/GPUCulling.h"
#include "Pipeline/GPUScene.h"
#include "SceneRenderer.h"
#include "Math/Math.h"
#include <vector>
#include <memory>

namespace he::render {

// ============================================================
// GBuffer 渲染上下文（CPU/GPU 模式共用）
// ============================================================
struct GBufferContext {
    rhi::IRHIDevice* device = nullptr;
    u32 width  = 0;
    u32 height = 0;

    // GBuffer 纹理
    rhi::IRHITexture* gbA     = nullptr;
    rhi::IRHITexture* gbB     = nullptr;
    rhi::IRHITexture* gbC     = nullptr;
    rhi::IRHITexture* gbVel      = nullptr;
    rhi::IRHITexture* gbDepth    = nullptr;
    rhi::IRHITexture* gbWorldPos = nullptr;  // MRT4: worldPos.xyz（RGBA16_FLOAT）

    // PSO + DescriptorSet
    rhi::IRHIPipelineState* pso     = nullptr;
    rhi::DescriptorSetHandle descSet = rhi::kInvalidSet;

    // Per-frame ObjectBuffer
    rhi::IRHIBuffer* objectBuffer = nullptr;

    // Scene + Culling
    SceneRenderer* sceneRenderer = nullptr;
    GPUCulling*    gpuCulling    = nullptr;
    GPUScene*      gpuScene      = nullptr;

    // CPU 可见索引（GPU 剔除 Readback 结果）
    const std::vector<u32>* gpuVisibleIndices = nullptr;

    // 上一帧 ViewProj（velocity 计算用）
    float4x4 prevViewProj = float4x4(1.0f);

    // GPU Driven 专用：MeshBatcher（VB/IB 合并，由 DeferredPipeline 管理生命周期）
    class MeshBatcher* meshBatcher = nullptr;

    // ── DGC 执行上下文（GPU Driven + DGC 模式使用）──
    struct DGCContext {
        bool    enabled               = false;  // DGC 是否启用
        void*   indirectCommandsLayout = nullptr; // VkIndirectCommandsLayoutEXT
        void*   indirectExecutionSet   = nullptr; // VkIndirectExecutionSetEXT
        u64     preprocessBufferAddr   = 0;       // 预处理缓冲 GPU 地址
        u64     preprocessBufferSize   = 0;       // 预处理缓冲大小
        u32     maxSequenceCount       = 0;       // 最大序列数
        rhi::IRHIBuffer* sequenceBuffer  = nullptr; // 间接绘制命令缓冲（由 GPU Cull 填充）
        rhi::IRHIBuffer* countBuffer     = nullptr; // 实际绘制数缓冲（由 GPU Cull 填充）
    };
    DGCContext dgc;
};

// ============================================================
// IGBufferRenderer — GBuffer 渲染接口
// ============================================================
class IGBufferRenderer {
public:
    virtual ~IGBufferRenderer() = default;

    /// 初始化（CPU 模式仅记录上下文引用；GPU 模式还做 MeshBatcher::Build）
    virtual bool Initialize(GBufferContext& ctx) = 0;
    virtual void Shutdown() = 0;

    /// 执行 GBuffer 渲染（BeginOffscreenPassMRT → 绘制 → EndOffscreenPass）
    virtual void Render(rhi::IRHICommandList* cmd, GBufferContext& ctx,
                        he::World& world, he::SceneGraph& sg,
                        const CameraData& camera) = 0;
};

} // namespace he::render
