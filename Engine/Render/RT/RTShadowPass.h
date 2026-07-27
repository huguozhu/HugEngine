#pragma once

#include "RHI/RHI.h"
#include "Math/Math.h"
#include <memory>

namespace he::render {

// ============================================================
// RTShadowPass — 硬件 Ray Tracing 阴影 Pass
//
// 对每个像素发射 shadow ray，输出单通道阴影遮罩纹理。
// 替代传统 CSM + Spot Shadow Maps。
//
// 特性:
//   - 无级联过渡伪影 (cascade blending artifacts)
//   - 无 shadow acne / peter panning
//   - 天然支持点光源 / 聚光灯阴影
//   - 可扩展为面光源软阴影
// ============================================================
class RTShadowPass {
    HE_DECLARE_NON_COPYABLE(RTShadowPass);

public:
    RTShadowPass()  = default;
    ~RTShadowPass() = default;

    // ── 生命周期 ──
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height, bool halfRes = true);
    void Shutdown();
    void OnResize(u32 width, u32 height);

    // ── 每帧执行 ──
    // 发射阴影射线，结果写入内部纹理
    void Execute(rhi::IRHICommandList* cmd,
                 rhi::IRHIAccelerationStructure* tlas,   // TLAS
                 rhi::IRHITexture* gbDepth,               // GBuffer 深度
                 rhi::IRHITexture* gbNormal,              // GBuffer 法线 (world space)
                 rhi::IRHIBuffer*  lightBuffer,           // GPULight[] SSBO
                 u32 lightCount,                          // 有效光源数
                 const float4x4& invViewProj,             // 逆 ViewProj（深度→世界坐标）
                 const float3& cameraPos,                 // 相机世界坐标
                 u32 frameIndex = 0);                     // 帧索引（时域抖动用）

    // ── 访问器 ──
    rhi::IRHITexture* GetShadowMask() const { return m_ShadowMask.get(); }
    u32 GetWidth()  const { return m_Width; }
    u32 GetHeight() const { return m_Height; }
    bool IsHalfRes() const { return m_HalfRes; }
    void SetHalfRes(bool hr) { m_HalfRes = hr; }

private:
    void CreateDescriptorSet(rhi::IRHIDevice* device);

    std::unique_ptr<rhi::IRHITexture> m_ShadowMask;   // 输出: 单通道阴影遮罩 (R16_FLOAT)
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;
    rhi::PushConstantRange         m_PCRange;

    u32 m_Width  = 0;
    u32 m_Height = 0;
    u32 m_FullWidth  = 0;   // 全分辨率宽（半分辨率模式下的原始分辨率）
    u32 m_FullHeight = 0;
    bool m_HalfRes = true;  // 默认半分辨率（阴影是低频效果）
};

} // namespace he::render
