#pragma once

#include "Pipeline/IRenderPipeline.h"
#include "Pipeline/Material.h"   // GPULight / MAX_LIGHTS / MAX_FRAMES_IN_FLIGHT
#include "Pipeline/RTPass.h"
#include "Pipeline/ParticleRenderer.h"
#include "Pipeline/PTQualityCVars.h"  // cvPTAtrous（A-Trous 访问器内联读写）
#include "RT/PTPass.h"
#include "RT/ReSTIRPass.h"
#include "RT/STBNTexture.h"
#include "PostProcess/PostProcessChain.h"
#include "PostProcess/RTDenoiser.h"
#include "PostProcess/PTAtrousPass.h"  // A-Trous 空间滤波（时域降噪之后）
#include "RHI/RHI.h"
#include "RenderGraph.h"
#include <memory>

namespace he::render {

// ============================================================
// PathTracingPipeline — 全路径追踪管线（Level 2: PT 参考渲染器）
//
// 【定位】参考渲染器 / Ground Truth —— 不是「第四种实时管线」。
//   · 它不参与实时渲染，也不使用 GI 层栈（GIConfig）：PT 自己完成完整光照；
//   · 收敛后结果是渲染方程的数值解（物理正确），用作其他近似 GI 的判定基准：
//       - 层栈归一化合成的能量是否守恒（白炉测试）
//       - DDGI 探针辐射度是否视角无关（M5.2-B）
//       - DDGI SH 评估是否需 cos 投影修正（M5.3）
//       - 各档位 GI 组合的亮度基准
//   · 与 DeferredPipeline 的关系：Deferred 是「待测对象」，PT 是「标准答案」。
//
// 与已移除的 HybridRTPipeline 的区别：HybridRT 只是「Deferred + RT 效果」的
// 配置差异（已并入层栈），而 PT 是完全不同的渲染范式（不做光栅化）。
//
// PT RayGen 直接渲染整帧：
//   [AS_Build] → [PT_Render（NEE + MIS + 俄罗斯轮盘赌）] → [ReSTIR DI（可选）]
//   → [PT_Denoise（时域累积）] → ToneMap → FXAA → BackBuffer
//
// 通过 r.Pipeline.Mode CVar / 02.Cube renderMode 切换 (3=PathTrace)。
// 无 GBuffer 光栅化：PT 输出 5 张 UAV（HDR 颜色 + 深度/法线/速度/albedoMetallic），
// 供时域降噪（RTDenoiser）、À-Trous 空间滤波与 ReSTIR_Init 使用。
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
    RTPass*               GetRTPass()    { return m_RTPass ? m_RTPass.get() : m_SharedRTPass; }
    PostProcessChain*     GetPostProcess() { return &m_PostProcess; }
    bool                  IsRTEnabled()  const { return m_RTEnabled; }

    /// 共享外部 RTPass（如 DeferredPipeline 的）：复用同一份 BLAS/TLAS，避免重复内存。
    /// 必须在 Initialize 之后调用；调用后释放自身实例，且不再自行构建加速结构。
    void SetSharedRTPass(RTPass* shared);

    // PT 质量开关（CVar 薄封装：读写 r.PT.* 开关，实现见 .cpp，供 ImGui / CVar 控制）
    void SetPTDenoise(bool e);
    bool IsPTDenoise() const;
    void SetPTAtrous(bool e) { cvPTAtrous.Set(e); }   // A-Trous 空间滤波开关（内联读写 CVar）
    bool IsPTAtrous() const  { return cvPTAtrous.Get(); }
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

    // GPU 粒子系统（PT 模式支持粒子渲染）
    ParticleRenderer& GetParticleRenderer() { return m_ParticleRenderer; }
    void AddParticleComponent(u32 id)       { m_ParticleComponentIDs.push_back(id); }

private:
    void BuildFrameGraph(RenderGraph& rg, he::World& world,
                         he::SceneGraph& sg, const CameraData& camera);
    void CollectLights(he::World& world, he::SceneGraph& sg,
                       const CameraData& camera, u32& outLightCount);

    rhi::IRHIDevice*    m_Device    = nullptr;
    rhi::IRHISwapChain* m_SwapChain = nullptr;

    // ── RT 基础设施 ──
    std::unique_ptr<RTPass> m_RTPass;    // AS 构建 + TLAS + 场景资源（AS-only；共享时为空）
    RTPass*                 m_SharedRTPass = nullptr;   // 外部共享的 RTPass（非拥有）
    std::unique_ptr<PTPass> m_PT;        // 全路径追踪 RayGen（4 输出 UAV）
    std::unique_ptr<ReSTIRPass> m_ReSTIR; // ReSTIR DI（阶段 B，始终创建）
    bool m_RTEnabled = false;

    // ── 时域降噪 ──
    std::unique_ptr<RTDenoiser> m_PTDenoiser;

    // ── A-Trous 空间滤波（时域降噪之后，边缘感知多迭代滤波）──
    std::unique_ptr<PTAtrousPass> m_PTAtrous;

    // ── STBN 时空蓝噪声（PT/ReSTIR 共用，3D 纹理 Load 采样）──
    std::unique_ptr<STBNTexture> m_STBN;

    // ── 后处理 ──
    PostProcessChain m_PostProcess;
    std::unique_ptr<rhi::IRHISampler> m_LinearSampler;  // ToneMap HDR 输入采样器

    // ── GPU 粒子系统（PT HDR 上复合 Billboard 粒子）──
    ParticleRenderer m_ParticleRenderer;
    std::vector<u32> m_ParticleComponentIDs;   // 注册的粒子组件 ID 列表
    std::unique_ptr<rhi::IRHITexture> m_ParticleDepth;       // 粒子深度附件（D32，每帧清远平面）
    std::unique_ptr<rhi::IRHISampler> m_ParticleDepthSampler; // 软粒子采样器（点采样）

    // ── 三缓冲光源 SSBO ──
    std::unique_ptr<rhi::IRHIBuffer> m_LightBuffers[MAX_FRAMES_IN_FLIGHT];
    u32 m_CurrentFrameSlot = 0;

    // 相机矩阵缓存（velocity 计算用）
    float4x4 m_PrevViewProj = float4x4(1.0f);
    float4x4 m_CurrViewProj = float4x4(1.0f);

    // 相机运动自适应降噪：上一帧相机位置/朝向，用于计算运动量并抬升时域混合权重
    float3 m_PrevCamPos = float3(0.0f);
    float3 m_PrevCamFwd = float3(0.0f, 0.0f, -1.0f);
    bool   m_CamInited  = false;
    u32 m_FrameIndex = 0;          // 帧索引（PT 抖动 / ReSTIR 种子）
    u32 m_PrevLightCount = 0;      // 上帧光源数（变化时 ReSTIR 历史失效）
    bool m_ReservoirReady = false; // ReSTIR 蓄水池可用（首帧/光源变化后一帧内为 false）
    bool m_SceneMaterialBuilt = false;  // 场景材质纹理是否已构建（延迟到首帧）

    u32 m_Width = rhi::kDefaultBackBufferWidth, m_Height = rhi::kDefaultBackBufferHeight;
    bool m_Ready = false;
};

} // namespace he::render
