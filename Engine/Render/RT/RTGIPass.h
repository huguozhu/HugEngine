#pragma once

#include "RT/RTEffectPass.h"
#include <memory>

namespace he::render {

// ============================================================
// RTGIPass — 硬件 Ray Tracing 间接漫反射（GI）Pass
//
// 对 GBuffer 有效像素在法线半球余弦采样 N 条射线（一次反弹），
// ClosestHit 评估命中表面出射辐射度，Miss 回退天空色。
// 替代 SSGI（屏幕空间间接光）——可覆盖屏幕外区域。
//
// 输出: RGBA16_FLOAT 四分之一分辨率（默认）间接漫反射
// （rgb + 有效性 a）。与 DDGI 配合：RT GI 覆盖中距离，DDGI 覆盖远距离。
// ============================================================
class RTGIPass : public RTEffectPass {
    HE_DECLARE_NON_COPYABLE(RTGIPass);

public:
    RTGIPass()  = default;
    ~RTGIPass() override = default;

    // 初始化（quarterRes=true 时输出四分之一分辨率）
    bool Initialize(rhi::IRHIDevice* device, u32 fullWidth, u32 fullHeight,
                    bool quarterRes = true);

    // 每帧执行：更新 set0 描述符 + 填充光源 UB + push constants + TraceRays
    void Execute(rhi::IRHICommandList* cmd,
                 rhi::IRHIAccelerationStructure* tlas,
                 const RTExecuteContext& ctx) override;

    // ── 访问器 ──
    bool IsQuarterRes() const { return m_QuarterRes; }
    void SetQuarterRes(bool qr) { m_QuarterRes = qr; }

private:
    u32 m_FullWidth = 0, m_FullHeight = 0;
    bool m_QuarterRes = true;    // 默认四分之一分辨率（GI 是极低频效果）
};

} // namespace he::render
