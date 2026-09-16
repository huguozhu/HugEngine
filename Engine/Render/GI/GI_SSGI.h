#pragma once

#include "GI/GlobalIllumination.h"
#include "RHI/RHI.h"
#include "Math/Math.h"
#include <memory>
#include <vector>

namespace he::render {

struct CameraData;   // 前向声明（与 GI_DDGI 同做法，避免头文件循环）

// ============================================================
// GI_SSGI — 屏幕空间全局光照（间接漫反射）
// 继承 IGlobalIllumination，纳入统一 GI 架构
// ============================================================
class GI_SSGI : public IGlobalIllumination {
public:
    GI_SSGI() = default;

    // IRenderSubsystem
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height) override;
    void Shutdown() override;
    void Update(const SubsystemContext&) override {}
    void Render(rhi::IRHICommandList* cmd) override;
    void Bind(rhi::IRHICommandList*) const override {}
    void OnResize(u32 w, u32 h) override;
    const char* GetName() const override { return "GI_SSGI"; }
    bool IsReady() const override { return m_Ready; }
    bool IsEnabled() const override { return m_Settings.enabled; }
    void SetEnabled(bool e) override { m_Settings.enabled = e; }

    // IGlobalIllumination
    GIMode GetMode() const override { return GIMode::SSGI; }
    rhi::IRHITexture* GetIndirectDiffuseTexture() const override { return m_Output.get(); }

    // SSGI 特有
    void SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal, rhi::IRHITexture* albedo);
    /// 注入真实相机（每帧由帧图给出）。屏幕空间重建必须用**渲染深度图时的那套**投影参数：
    /// 此前用硬编码的 kDefaultFOV/0.1/2000 自行拼投影矩阵，非默认相机（PhysicalCamera 会由
    /// 焦距反算 fov）下 viewPos 重建错位（§9.2-E）。同时视图矩阵用于把 GBuffer 的世界空间
    /// 法线转到 view 空间。传 nullptr 时退化为默认投影，保证独立运行该 pass 也不会拿到未初始化矩阵。
    void SetCamera(const CameraData* camera) { m_Camera = camera; }
    rhi::IRHISampler* GetOutputSampler() const { return m_Sampler.get(); }
    void PreBind(rhi::IRHICommandList* cmd) const { if (m_Ready) cmd->SetPipeline(m_PSO.get()); }

    float radius = 1.0f;
    int   sampleCount = 16;

private:
    void CreateOutputTex(u32 w, u32 h);
    // 半分辨率尺寸（GISettings.halfRes 时输出纹理降半——省约 3/4 像素着色）
    u32 halfResW(u32 w) const { return m_Settings.halfRes ? std::max(w / 2, 1u) : w; }
    u32 halfResH(u32 h) const { return m_Settings.halfRes ? std::max(h / 2, 1u) : h; }

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false;

    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_DescLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_DescSet    = rhi::kInvalidSet;

    std::unique_ptr<rhi::IRHITexture> m_Output;
    std::unique_ptr<rhi::IRHISampler> m_Sampler;
    std::unique_ptr<rhi::IRHISampler> m_PointSampler;

    // Uniform Buffer：存储采样内核 + 参数（绑定到 binding 3，替代 push constant）
    std::unique_ptr<rhi::IRHIBuffer> m_UniformBuffer;

    rhi::IRHITexture* m_Depth   = nullptr;
    rhi::IRHITexture* m_Albedo  = nullptr;
    rhi::IRHITexture* m_Normal  = nullptr;
    const CameraData* m_Camera  = nullptr;   // 非拥有；帧图每帧注入（见 SetCamera）
};

} // namespace he::render
