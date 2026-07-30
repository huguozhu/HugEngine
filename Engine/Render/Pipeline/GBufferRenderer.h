#pragma once

#include "RHI/RHI.h"
#include "Pipeline/Material.h"
#include "Pipeline/GPUCulling.h"
#include "Pipeline/GPUScene.h"
#include "SceneRenderer.h"
#include "RenderGraph.h"
#include "Math/Math.h"
#include <vector>
#include <memory>

namespace he::render {

// GBuffer 附件布局常量
constexpr u32 kGBufferAttachmentCount = 5;
constexpr u32 kGBufferSlotAlbedo      = 0;  // Albedo.rgb + Metallic.a（RGBA16_FLOAT）
constexpr u32 kGBufferSlotNormal      = 1;  // Normal.xyz + Roughness.a（RGBA16_FLOAT）
constexpr u32 kGBufferSlotEmissive    = 2;  // Emissive.rgb + AO.a（RGBA16_FLOAT）
constexpr u32 kGBufferSlotVelocity    = 3;  // Velocity.xy（RG16_FLOAT）
constexpr u32 kGBufferSlotWorldPos    = 4;  // WorldPos.xyz（RGBA16_FLOAT）

// ============================================================
// GBuffer 渲染上下文（CPU/GPU 模式共用，内部实现细节）
// ============================================================
struct GBufferContext {
    rhi::IRHIDevice* device = nullptr;
    u32 width  = 0;
    u32 height = 0;

    // GBuffer 纹理（裸指针，由 GBufferRenderer 管理生命周期）
    rhi::IRHITexture* gbA        = nullptr;
    rhi::IRHITexture* gbB        = nullptr;
    rhi::IRHITexture* gbC        = nullptr;
    rhi::IRHITexture* gbVel      = nullptr;
    rhi::IRHITexture* gbDepth    = nullptr;
    rhi::IRHITexture* gbWorldPos = nullptr;  // MRT4: worldPos.xyz（RGBA16_FLOAT）

    // PSO + DescriptorSet（由 GBufferRenderer 管理）
    rhi::IRHIPipelineState* pso     = nullptr;
    rhi::DescriptorSetHandle descSet = rhi::kInvalidSet;

    // Per-frame ObjectBuffer（每帧由外部设置）
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
// IGBufferRenderer — GBuffer 渲染接口（策略模式）
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

// ============================================================
// GBufferRenderer — GBuffer 纹理所有权 + 渲染（共享组件）
//
// 拥有 5 个 MRT 颜色纹理 + 深度纹理，提供 ImportToRenderGraph
// 和 Render 接口。内部委托给 IGBufferRenderer（CPU/GPU 策略）。
// 供 DeferredPipeline 使用。
// ============================================================
class GBufferRenderer {
public:
    // GBuffer 模式（CPU 逐物体绘制 / GPU Driven ExecuteIndirect）
    enum class Mode : u8 { CPU, GPU };

    GBufferRenderer()  = default;
    ~GBufferRenderer() = default;

    // ── 生命周期 ──
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();
    void OnResize(u32 width, u32 height);

    // ── RenderGraph 导入 ──
    // 将所有 GBuffer 纹理导入 RenderGraph，返回 ResourceHandle 集合
    struct Handles {
        ResourceHandle albedo;    // RGBA16_FLOAT  (baseColor.rgb + metallic)
        ResourceHandle normal;    // RGBA16_FLOAT  (worldNormal.xyz + roughness)
        ResourceHandle emissive;  // RGBA16_FLOAT  (emissive.rgb + ao)
        ResourceHandle velocity;  // RG16_FLOAT    (screen-space motion vector)
        ResourceHandle worldPos;  // RGBA16_FLOAT  (worldPos.xyz)
        ResourceHandle depth;     // D32_FLOAT
    };
    Handles ImportToRenderGraph(RenderGraph& rg);

    // ── 渲染 ──
    // 执行 GBuffer 渲染（委托给 IGBufferRenderer::Render）
    void Render(rhi::IRHICommandList* cmd, he::World& world,
                he::SceneGraph& sg, const CameraData& camera);

    // ── 每帧动态参数设置（在 Render 之前调用）──
    void SetObjectBuffer(rhi::IRHIBuffer* objBuf)  { m_Ctx.objectBuffer = objBuf; }
    void SetPrevViewProj(const float4x4& m)        { m_Ctx.prevViewProj = m; }
    void SetSceneRenderer(SceneRenderer* sr)       { m_Ctx.sceneRenderer = sr; }
    void SetGPUCulling(GPUCulling* gc)             { m_Ctx.gpuCulling = gc; }
    void SetGPUScene(GPUScene* gs)                 { m_Ctx.gpuScene = gs; }
    void SetVisibleIndices(const std::vector<u32>* vi) { m_Ctx.gpuVisibleIndices = vi; }
    void SetMeshBatcher(MeshBatcher* mb)           { m_Ctx.meshBatcher = mb; }
    void SetDGCContext(const GBufferContext::DGCContext& dgc) { m_Ctx.dgc = dgc; }
    void ClearDGCContext()                         { m_Ctx.dgc = {}; }

    // ── 配置 ──
    void SetMode(Mode m);
    Mode GetMode() const { return m_Mode; }
    GBufferContext& GetContext() { return m_Ctx; }

    // ── 纹理访问器（供 Lighting / PostProcess 使用）──
    rhi::IRHITexture* GetAlbedo()   const { return m_A.get(); }
    rhi::IRHITexture* GetNormal()   const { return m_B.get(); }
    rhi::IRHITexture* GetEmissive() const { return m_C.get(); }
    rhi::IRHITexture* GetVelocity() const { return m_D.get(); }
    rhi::IRHITexture* GetWorldPos() const { return m_E.get(); }
    rhi::IRHITexture* GetDepth()    const { return m_Depth.get(); }

    // ── 描述符集访问器 ──
    rhi::IRHIPipelineState*       GetPSO()           const { return m_PSO.get(); }
    rhi::DescriptorSetLayoutHandle GetLayout()       const { return m_Layout; }
    rhi::DescriptorSetHandle       GetDescriptorSet() const { return m_Set; }

private:
    void CreateTextures(rhi::IRHIDevice* device);
    void CreatePSO(rhi::IRHIDevice* device);
    void CreateDescriptorSet(rhi::IRHIDevice* device);

    // ── 纹理所有权 ──
    std::unique_ptr<rhi::IRHITexture> m_A;       // GBufferA: albedo.rgb + metallic.a
    std::unique_ptr<rhi::IRHITexture> m_B;       // GBufferB: normal.xyz + roughness.a
    std::unique_ptr<rhi::IRHITexture> m_C;       // GBufferC: emissive.rgb + ao.a
    std::unique_ptr<rhi::IRHITexture> m_D;       // GBufferD: velocity.xy (RG16_FLOAT)
    std::unique_ptr<rhi::IRHITexture> m_E;       // GBufferE: worldPos.xyz
    std::unique_ptr<rhi::IRHITexture> m_Depth;   // 深度缓冲 (D32_FLOAT)

    // ── PSO + 描述符集 ──
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;

    // ── 策略模式渲染器 ──
    std::unique_ptr<IGBufferRenderer> m_Renderer;

    // ── 上下文（传递给 IGBufferRenderer::Render）──
    GBufferContext m_Ctx;

    Mode m_Mode   = Mode::CPU;
    u32  m_Width  = 0;
    u32  m_Height = 0;
};

} // namespace he::render
