#pragma once

#include "RT/RTEffectPass.h"
#include <memory>

namespace he::render {

// ============================================================
// RTReflectionPass — 硬件 Ray Tracing 反射 Pass
//
// 对 GBuffer 像素按粗糙度发射反射光线（GGX 重要性采样），
// ClosestHit 评估命中表面出射辐射度，Miss 回退程序化天空色。
// 替代 SSR（屏幕空间反射）——可反射屏幕外物体。
//
// 输出: RGBA16_FLOAT 半分辨率（默认）反射颜色（rgb + 命中距离 a）
// 分辨率策略（P2）：半分辨率固定（完整策略：粗糙度分级分辨率见规划）
// ============================================================
class RTReflectionPass : public RTEffectPass {
    HE_DECLARE_NON_COPYABLE(RTReflectionPass);

public:
    RTReflectionPass()  = default;
    ~RTReflectionPass() override = default;

    // 初始化（halfRes=true 时输出半分辨率反射）
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
    u32 m_FullWidth = 0, m_FullHeight = 0;
    bool m_HalfRes = true;       // 默认半分辨率（反射是低频效果）
};

} // namespace he::render
