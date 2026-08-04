#pragma once

#include "Pipeline/IRenderPipeline.h"
#include "Pipeline/Material.h"   // GPULight / MAX_LIGHTS / MAX_FRAMES_IN_FLIGHT
#include "Pipeline/RTPass.h"
#include "RT/PTPass.h"
#include "RT/ReSTIRPass.h"
#include "PostProcess/PostProcessChain.h"
#include "PostProcess/RTDenoiser.h"
#include "RHI/RHI.h"
#include "RenderGraph.h"
#include <memory>

namespace he::render {

// ============================================================
// PathTracingPipeline — 全路径追踪管线（Level 2: PT 参考模式）
//
// 与 DeferredPipeline / HybridRTPipeline 平行。PT RayGen 直接渲染整帧：
//   [AS_Build] → [PT_Render（NEE+MIS+俄罗斯轮盘赌）] → [ReSTIR DI（可选）]
//   → [PT_Denoise（时域累积）] → ToneMap → FXAA → BackBuffer
//
// 通过 r.Pipeline.Mode CVar / 02.Cube renderMode 切换 (3=PathTrace)。
// 无 GBuffer 光栅化：PT 输出 4 张 UAV（HDR 颜色 + 深度/法线/速度），
// 供时域降噪（RTDenoiser）与 ReSTIR_Init 重建世界坐标使用。
// ============================================================
class PathTracingPipeline : public IRenderPipeline {
    HE_DECLARE_NON_COPYABLE(PathTracingPipeline);

public:
    PathTracingPipeline()  = default;
    ~PathTracingPipeline() override = default;

    bool Initialize(rhi::IRHIDevice* device) override;
    void Shutdown() override;
    void NextFrame() override;
    void OnResize(u32 width, u32 height) override;
    const char* GetName() const override { return "PathTracingPipeline"; }

    void Render(rhi::IRHICommandList* cmd, he::World& world,
                he::SceneGraph& sg, const CameraData& camera,
                float deltaTime = 0.016f) override;

    // ── 访问器（供 02.Cube 的 ImGui 调用）──
    void SetSwapChain(rhi::IRHISwapChain* sc) { m_SwapChain = sc; }
    PTPass*               GetPT()        { return m_PT.get(); }
    RTPass*               GetRTPass()    { return m_RTPass.get(); }
    PostProcessChain*     GetPostProcess() { return &m_PostProcess; }
    bool                  IsRTEnabled()  const { return m_RTEnabled; }

    // PT 质量开关（CVar 薄封装：读写 r.PT.* 开关，实现见 .cpp，供 ImGui / CVar 控制）
    void SetPTDenoise(bool e);
    bool IsPTDenoise() const;
    void SetPTReSTIR(bool e);
    bool IsPTReSTIR() const;
    void SetPTMIS(bool e);
    bool IsPTMIS() const;
    void SetPTRoulette(bool e);
    bool IsPTRoulette() const;
    void SetPTSampleCount(i32 v);
    i32  GetPTSampleCount() const;
    void SetPTMaxBounces(i32 v);
    i32  GetPTMaxBounces() const;

    // ReSTIR 判定（供 BuildFrameGraph / ImGui 查询当前蓄水池是否可用）
    bool IsReservoirReady() const { return m_ReservoirReady; }

private:
    void BuildFrameGraph(RenderGraph& rg, he::World& world,
                         he::SceneGraph& sg, const CameraData& camera);
    void CollectLights(he::World& world, he::SceneGraph& sg,
                       const CameraData& camera, u32& outLightCount);

    rhi::IRHIDevice*    m_Device    = nullptr;
    rhi::IRHISwapChain* m_SwapChain = nullptr;

    // ── RT 基础设施 ──
    std::unique_ptr<RTPass> m_RTPass;    // AS 构建 + TLAS + 场景资源（AS-only）
    std::unique_ptr<PTPass> m_PT;        // 全路径追踪 RayGen（4 输出 UAV）
    std::unique_ptr<ReSTIRPass> m_ReSTIR; // ReSTIR DI（阶段 B，始终创建）
    bool m_RTEnabled = false;

    // ── 时域降噪 ──
    std::unique_ptr<RTDenoiser> m_PTDenoiser;

    // ── 后处理 ──
    PostProcessChain m_PostProcess;
    std::unique_ptr<rhi::IRHISampler> m_LinearSampler;  // ToneMap HDR 输入采样器

    // ── 三缓冲光源 SSBO ──
    std::unique_ptr<rhi::IRHIBuffer> m_LightBuffers[MAX_FRAMES_IN_FLIGHT];
    u32 m_CurrentFrameSlot = 0;

    // 相机矩阵缓存（velocity 计算用）
    float4x4 m_PrevViewProj = float4x4(1.0f);
    float4x4 m_CurrViewProj = float4x4(1.0f);
    u32 m_FrameIndex = 0;          // 帧索引（PT 抖动 / ReSTIR 种子）
    u32 m_PrevLightCount = 0;      // 上帧光源数（变化时 ReSTIR 历史失效）
    bool m_ReservoirReady = false; // ReSTIR 蓄水池可用（首帧/光源变化后一帧内为 false）
    bool m_SceneMaterialBuilt = false;  // 场景材质纹理是否已构建（延迟到首帧）

    u32 m_Width = rhi::kDefaultBackBufferWidth, m_Height = rhi::kDefaultBackBufferHeight;
    bool m_Ready = false;
};

} // namespace he::render
