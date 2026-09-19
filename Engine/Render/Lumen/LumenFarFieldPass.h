#pragma once

// ============================================================
// Lumen/LumenFarFieldPass.h — Lumen 远场硬件光追（步骤 26）
//
// 【它在 Lumen 里的位置】步骤 21 用 SDF sphere tracing 求交，步骤 26 起同一条光线再打一次
// **硬件光追**：同一位置、同一方向（方向来自共享的 `ScreenProbeSampling.slang`），把命中距离与
// 命中辐射度写进缓冲。近场仍由 SDF 负责，**切换与淡入是步骤 27**；本步先把"远场到底命中了什么"
// 量出来，并给出"SDF 与三角形场景是否一致"的验收读数。
//
// 【为什么复用 RTGI 的 rchit/rmiss】远场命中的辐射度必须与 RTGI 同源同量纲，否则步骤 27 的
// 切换会在阈值处出现阶跃。命中着色（场景材质 + 直接光）整套逻辑都在 `RT_GI.rchit` 里。
//
// 【为什么继承 RTEffectPass】复用它的 RT 管线 + SBT + set0 生命周期（与 `DDGITracePass` 同一
// 取舍）。代价是基类会顺便建一张 1×1 占位输出纹理（本 pass 的输出是 SSBO，那张纹理从不被用）。
// ============================================================

#include "RT/RTEffectPass.h"

#include <memory>

namespace he::render {

class LumenFarFieldPass : public RTEffectPass {
    HE_DECLARE_NON_COPYABLE(LumenFarFieldPass);

public:
    LumenFarFieldPass() = default;
    ~LumenFarFieldPass() override = default;

    bool Initialize(rhi::IRHIDevice* device);

    /// 每帧参数（都在这里显式给出，避免依赖"上一帧设过"的隐式状态）
    struct FrameParams {
        rhi::IRHIBuffer*  probeBuffer   = nullptr;   // StructuredBuffer<ScreenProbe>
        u32               probeCount    = 0;
        u32               traceRep      = 8;         // 每探针光线数（= 步骤 21 的 traceRep）
        u32               seed          = 0;         // 随机种子（必须与 SDF 追踪那一帧的种子一致）
        u32               sampleMode    = 1;         // 0=GGX, 1=均匀半球（与 SDF 追踪同值）
        float             tMin          = 0.0f;
        float             tMax          = 500.0f;
        float             farThreshold  = 50.0f;     // 计划的"远场阈值"（本步只记录，切换在步骤 27）
        rhi::IRHITexture* materialTex   = nullptr;   // 场景材质纹理（rchit 用）
        rhi::IRHITexture* triangleNorms = nullptr;   // 三角形法线（rchit 用）
        rhi::IRHIBuffer*  lightBuffer   = nullptr;   // GPULight[]（rchit 直接光）
        u32               lightCount    = 0;
    };

    /// 发射全部远场光线。调用方负责保证 TLAS 已在本帧构建完成。
    /// 返回本帧发射的射线数（0 表示未发射）。
    u32 Trace(rhi::IRHICommandList* cmd, rhi::IRHIAccelerationStructure* tlas, const FrameParams& p);

    /// 基类要求的入口：本 pass 由 `LumenScene::RunFarFieldRT` 直接驱动，不挂 GI Provider，
    /// 因此这里只做一次显式告警（写死"不实现"比静默什么都不做好）。
    void Execute(rhi::IRHICommandList* /*cmd*/, rhi::IRHIAccelerationStructure* /*tlas*/,
                 const RTExecuteContext& /*ctx*/) override;

    [[nodiscard]] rhi::IRHIBuffer* GetResultBuffer() const { return m_Result.get(); }
    [[nodiscard]] const void*      GetResultMapped() const { return m_ResultMapped; }
    [[nodiscard]] u32              GetRayCount() const { return m_RayCount; }
    [[nodiscard]] bool IsValid() const { return RTEffectPass::IsValid() && m_Result != nullptr; }

private:
    // set0 绑定号（与 Lumen_FarField.rgen.slang 一致；4/5/6 由 RT_HitCommon.slang 固定占用）
    static constexpr u32 kBindTLAS     = 0;
    static constexpr u32 kBindResult   = 1;
    static constexpr u32 kBindProbes   = 2;
    static constexpr u32 kBindMaterial = 4;
    static constexpr u32 kBindLights   = 5;
    static constexpr u32 kBindNormals  = 6;
    static constexpr u32 kBindParams   = 7;

    // 参数 UBO（与 `LumenFarField.slang` 的 LumenFarFieldParams 逐字段一致）
    struct FarFieldParams {
        u32   probeCount;
        u32   traceRep;
        u32   seed;
        u32   sampleMode;
        float tMin;
        float tMax;
        float farThreshold;
        float pad;
    };
    static_assert(sizeof(FarFieldParams) == 32, "LumenFarFieldParams 必须是 32 B（与 shader 一致）");

    bool EnsureCapacity(u32 rayCount);

    std::unique_ptr<rhi::IRHIBuffer> m_Result;        // RWStructuredBuffer<float4>（每光线一条）
    std::unique_ptr<rhi::IRHIBuffer> m_ParamsUBO;
    void*                            m_ResultMapped = nullptr;   // 直接映射读回（统计用）
    u32                              m_RayCount     = 0;
    u32                              m_Capacity     = 0;
    bool                             m_WarnedExecute = false;
};

} // namespace he::render
