#pragma once

#include "GI/GlobalIllumination.h"
#include "RHI/RHI.h"
#include "Math/Math.h"
#include <memory>

namespace he::render {

// ============================================================
// GI_DDGI — 动态漫反射全局光照（探针网格 + SH 辐照度）
//
// 3D 探针网格 → Compute Shader 每帧采样 GBuffer 更新 SH
// → Lighting shader 三线性插值采样间接漫反射。
//
// 探针数据布局（StructuredBuffer，每探针 16 float4）：
//   [0..8]  SH 系数 (band 0/1/2, 9×float4)
//   [9..15] 保留（深度/可见性/偏移等，暂未使用）
// ============================================================
class GI_DDGI : public IGlobalIllumination {
public:
    GI_DDGI() = default;

    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) override;
    void Shutdown() override;
    void Update(const SubsystemContext& ctx) override;
    void Render(rhi::IRHICommandList* cmd) override;
    void Bind(rhi::IRHICommandList*) const override {}
    void OnResize(u32 w, u32 h) override;
    const char* GetName() const override { return "GI_DDGI"; }
    bool IsReady() const override { return m_Ready; }
    bool IsEnabled() const override { return m_Settings.enabled; }
    void SetEnabled(bool e) override { m_Settings.enabled = e; }

    GIMode GetMode() const override { return GIMode::DDGI; }
    rhi::IRHITexture* GetIndirectDiffuseTexture() const override { return nullptr; }

    void SetGBufferInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal, rhi::IRHITexture* albedo);

    // 捕获当前帧 HDR 到前帧纹理（供下帧 DDGI 探针采样真实辐射度）
    void CaptureHDR(rhi::IRHICommandList* cmd, rhi::IRHITexture* hdr);

    // 探针数据缓冲（供 Lighting Pass 绑定，每帧更新后为最新 blend 结果）
    rhi::IRHIBuffer* GetProbeBuffer() const { return m_ProbeBuffer.get(); }

    // 探针网格参数
    u32 gridX = 8, gridY = 4, gridZ = 8;
    float3 gridOrigin = float3(-10, -2, -10);
    float  cellSize     = 3.0f;
    float  blendAlpha   = 0.85f;   // 时间混合：历史保留比例（0=无历史, 1=完全历史）
    float  debugScale   = 0.5f;    // 调试：DDGI 贡献缩放

private:
    // 每探针存储的 float4 数量（9 SH + 7 保留）
    static constexpr u32 kFloats4PerProbe = 16;

    // 探针网格参数 uniform 结构（与 shader 中 ProbeGridParams 保持一致）
    // 需满足 std140 对齐：float4=16B, float4x4=64B
    struct ProbeGridUniform {
        float4   gridOrigin;    // xyz=世界空间原点, w=未使用
        float4   gridSize;      // xyz=探针数量(gridX,gridY,gridZ), w=cellSize
        float4   cameraPos;     // xyz=相机世界位置, w=未使用
        float4   params;        // x=intensity, y=numSamples, z=blendAlpha, w=historyValid
        float4x4 viewProj;      // 相机 View→Proj（世界→裁剪，用于探针→屏幕投影）
    };

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false;

    // Compute Pipeline
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;

    // 探针数据（SSBO：每探针 16×float4，当前帧 blend 结果）
    std::unique_ptr<rhi::IRHIBuffer> m_ProbeBuffer;
    // 上一帧探针历史（SSBO，时间混合源）
    std::unique_ptr<rhi::IRHIBuffer> m_ProbeHistory;

    // 探针网格参数 Uniform Buffer
    std::unique_ptr<rhi::IRHIBuffer> m_GridUniform;

    // 采样器
    std::unique_ptr<rhi::IRHISampler> m_PointSampler;    // 点采样（GBuffer 读取）
    std::unique_ptr<rhi::IRHISampler> m_LinearSampler;   // 线性采样（前帧 HDR 读取）

    // 前帧 HDR 辐射度纹理（上个帧的 Lighting 输出，供探针采真实辐照度）
    std::unique_ptr<rhi::IRHITexture> m_PrevHDR;
    rhi::DescriptorSetLayoutHandle m_PrevHDR_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_PrevHDR_Set    = rhi::kInvalidSet;  // set=0 的追加描述符

    // GBuffer 输入（不持有所有权）
    rhi::IRHITexture* m_Depth  = nullptr;
    rhi::IRHITexture* m_Normal = nullptr;
    rhi::IRHITexture* m_Albedo = nullptr;

    // 相机
    bool m_CameraReady = false;
    float3 m_CameraPos;
    float4x4 m_ViewProj = float4x4(1.0f);  // 每帧从 CameraData 更新
};

} // namespace he::render
