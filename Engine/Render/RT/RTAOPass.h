#pragma once

#include "RT/RTEffectPass.h"
#include <memory>

namespace he::render {

// ============================================================
// RTAOPass — 硬件 Ray Tracing 环境光遮蔽 Pass
//
// 对 GBuffer 有效像素在法线半球余弦采样 N 条射线，
// 命中即遮蔽（复用 RT_Shadow.rahit AnyHit + RT_Common.rmiss），
// 输出 1-遮蔽率 到半分辨率 R8_UNORM 遮罩。替代 SSAO。
//
// 特性: 不限于屏幕空间，更物理准确；无 SSAO 的半径/采样伪影。
// ============================================================
class RTAOPass : public RTEffectPass {
    HE_DECLARE_NON_COPYABLE(RTAOPass);

public:
    RTAOPass()  = default;
    ~RTAOPass() override = default;

    // 初始化（halfRes=true 时输出半分辨率 AO 遮罩）
    bool Initialize(rhi::IRHIDevice* device, u32 fullWidth, u32 fullHeight,
                    bool halfRes = true);

    // 每帧执行：更新 set0 描述符 + push constants + TraceRays
    void Execute(rhi::IRHICommandList* cmd,
                 rhi::IRHIAccelerationStructure* tlas,
                 const RTExecuteContext& ctx) override;

    // ── 访问器 ──
    bool IsHalfRes() const { return m_HalfRes; }
    void SetHalfRes(bool hr) { m_HalfRes = hr; }

private:
    u32 m_FullWidth = 0, m_FullHeight = 0;
    bool m_HalfRes = true;       // 默认半分辨率（AO 是低频效果）
};

} // namespace he::render
