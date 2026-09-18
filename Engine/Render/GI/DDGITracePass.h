#pragma once

#include "RT/RTEffectPass.h"
#include <memory>

namespace he::render {

// ============================================================
// DDGITracePass — DDGI 探针射线的硬件光追 march（任务 17 / B4 · M5.2-A）
//
// 为每条探针射线（探针数 × 每探针采样数）追踪一条世界空间射线，把辐射度写进
// `RWStructuredBuffer<float4>`，供 DDGI.comp 的 SH 投影读取。命中用 RT_GI.rchit
// 评估命中点出射辐射度（与 RTGI 同一套，保证两个源的量纲一致），未命中取 IBL
// 辐照度图按方向的值（与改造前 DDGI 的回退同源）。
//
// 【为什么单独一个 pass，而不是塞进 GI_DDGI】DDGI 的探针更新是 compute pass，
// 而本 pass 是 ray tracing dispatch：两者的描述符集、管线、SBT 都不同；更重要的是
// **执行顺序**：本 pass 必须在 DDGI 的 compute 之前跑完（它写的缓冲就是 compute 的输入）。
//
// 【继承 RTEffectPass 的原因与代价】复用它的 RT 管线 + SBT + set0 生命周期管理
// （`CreateEffectPipeline` / `BindRT` / `TraceRays` / `CreateHitLightUB`）。代价是基类
// 会顺便创建一张**输出纹理**——本 pass 的输出是 SSBO，故那张纹理只是 1×1 占位，
// 从不被写也从不被采样（`GetOutput()` 不被使用）。
// ============================================================
class DDGITracePass : public RTEffectPass {
    HE_DECLARE_NON_COPYABLE(DDGITracePass);

public:
    DDGITracePass() = default;
    ~DDGITracePass() override = default;

    bool Initialize(rhi::IRHIDevice* device);

    /// 确保辐射度缓冲能装下 probeCount × samplesPerProbe 条射线（探针数随网格拟合变化，
    /// 变化时重建缓冲——与 DDGI 探针缓冲的重建同一时机）。
    bool EnsureCapacity(u32 probeCount, u32 samplesPerProbe);

    /// 每帧参数：网格（与 DDGI 的 ProbeGridUniform 前两个字段同源）、最大追踪距离、
    /// 探针步长占格距的比例（与 DDGI.comp 的 stepDist 一致，避免探针落在几何内部时自交）
    void SetGrid(const float3& origin, u32 countX, u32 countY, u32 countZ, float cellSize);
    void SetSampling(u32 samplesPerProbe, float maxDistance, float stepRatio);
    /// 未命中回退用的 IBL 辐照度（与 DDGI 探针更新的回退同一张图）
    void SetIBL(rhi::IRHITexture* irradiance, rhi::IRHISampler* sampler);

    /// 发射全部探针射线。调用方负责保证 TLAS 已在本帧构建完成。
    void Execute(rhi::IRHICommandList* cmd,
                 rhi::IRHIAccelerationStructure* tlas,
                 const RTExecuteContext& ctx) override;

    rhi::IRHIBuffer* GetRadianceBuffer() const { return m_Radiance.get(); }
    u32 GetRayCount() const { return m_RayCount; }
    u32 ProbeCapacity() const { return m_ProbeCapacity; }
    bool IsValid() const { return RTEffectPass::IsValid() && m_Radiance != nullptr; }

private:
    // set0 绑定号（与 DDGI_Trace.rgen.slang 一致；4/5/6 由 RT_HitCommon.slang 固定占用）
    static constexpr u32 kBindTLAS      = 0;
    static constexpr u32 kBindRadiance  = 1;
    static constexpr u32 kBindMaterial  = 4;
    static constexpr u32 kBindLights    = 5;
    static constexpr u32 kBindNormals   = 6;
    static constexpr u32 kBindParams    = 7;
    static constexpr u32 kBindIBL       = 8;

    // 参数 UBO（与 shader 的 DDGITraceParams 逐字段一致）
    struct TraceParams {
        float4 gridOrigin;   // xyz=原点
        float4 gridSize;     // xyz=探针数量, w=格距
        float4 params;       // x=每探针采样数, y=最大追踪距离, z=起始偏移系数
        float4 probeStep;    // x=步长占格距比例
    };

    std::unique_ptr<rhi::IRHIBuffer> m_Radiance;      // RWStructuredBuffer<float4>
    std::unique_ptr<rhi::IRHIBuffer> m_ParamsUBO;
    rhi::IRHITexture* m_IBLIrradiance = nullptr;
    rhi::IRHISampler* m_IBLSampler    = nullptr;

    TraceParams m_Params{};
    u32 m_SamplesPerProbe = 32;
    u32 m_ProbeCapacity   = 0;   // 已分配的探针容量（射线数 = 容量 × 每探针采样数）
    u32 m_RayCount        = 0;   // 本帧实际发射的射线数
    float m_MaxDistance   = 100.0f;
    float m_StepRatio     = 0.4f;
    bool m_ParamsDirty    = true;
};

} // namespace he::render
