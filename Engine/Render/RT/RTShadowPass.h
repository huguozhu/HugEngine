#pragma once

#include "RT/RTEffectPass.h"
#include <memory>

namespace he::render {

// ============================================================
// RTShadowPass — 硬件 Ray Tracing 阴影 Pass
//
// 对每个像素向每个阴影光源发射 shadow ray，输出单通道阴影遮罩。
// 替代传统 CSM + Spot Shadow Maps。
//
// 特性:
//   - 无级联过渡伪影 (cascade blending artifacts)
//   - 无 shadow acne / peter panning
//   - 天然支持点光源 / 聚光灯阴影
//   - 可扩展为面光源软阴影
//
// 输出: R16_FLOAT 半分辨率（默认）阴影遮罩（0=全影, 1=无影）
// 阴影语义: RayGen 用 ACCEPT_FIRST_HIT + AnyHit 置 blocked=true，
//           Miss 保持 blocked=false → 命中即遮挡，无最近命中开销
// ============================================================
class RTShadowPass : public RTEffectPass {
    HE_DECLARE_NON_COPYABLE(RTShadowPass);

public:
    RTShadowPass()  = default;
    ~RTShadowPass() override = default;

    // 初始化（halfRes=true 时输出半分辨率遮罩）
    bool Initialize(rhi::IRHIDevice* device, u32 fullWidth, u32 fullHeight,
                    bool halfRes = true);

    // 每帧执行：更新 set0 描述符 + 填充光源 UB + push constants + TraceRays
    void Execute(rhi::IRHICommandList* cmd,
                 rhi::IRHIAccelerationStructure* tlas,
                 const RTExecuteContext& ctx) override;

    // ── 访问器 ──
    bool IsHalfRes() const { return m_HalfRes; }
    void SetHalfRes(bool hr) { m_HalfRes = hr; }

private:
    // 从 GPULight[] SSBO 显式抽取阴影光源数据（48B/light，匹配 shader ShadowLight 布局）
    void FillLightBuffer(const RTExecuteContext& ctx);

    std::unique_ptr<rhi::IRHIBuffer> m_LightUB;   // 阴影光源 Uniform Buffer（48B × 16）
    u32 m_FullWidth = 0, m_FullHeight = 0;
    bool m_HalfRes = true;       // 默认半分辨率（阴影是低频效果）
};

} // namespace he::render
