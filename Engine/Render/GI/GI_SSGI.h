#pragma once

#include "GI/GlobalIllumination.h"
#include "GI/GIRadianceHistory.h"   // 前帧 HDR 辐射度（共享组件，入射辐射度来源）
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
    /// 让输出纹理尺寸与当前设置一致（`halfRes` 是运行时开关）。
    /// 必须在帧图**构图之前**调用（Provider::SyncToStack 正是这个时机）：否则本帧导入渲染图
    /// 的句柄会指向旧尺寸纹理，而设置要等到下次 OnResize 才生效（§9.2-G 的第三重真值）。
    void SyncOutputSize();
    /// 注入真实相机（每帧由帧图给出）。屏幕空间重建必须用**渲染深度图时的那套**投影参数：
    /// 此前用硬编码的 kDefaultFOV/0.1/2000 自行拼投影矩阵，非默认相机（PhysicalCamera 会由
    /// 焦距反算 fov）下 viewPos 重建错位（§9.2-E）。同时视图矩阵用于把 GBuffer 的世界空间
    /// 法线转到 view 空间。传 nullptr 时退化为默认投影，保证独立运行该 pass 也不会拿到未初始化矩阵。
    void SetCamera(const CameraData* camera) { m_Camera = camera; }
    /// 注入共享的「前帧 HDR 辐射度」组件（非拥有；须在 Initialize 之前调用）。
    /// 这是 SSGI 从「反照率启发式」变成 `E/π` 估计的**唯一**颜色输入：命中点处的入射
    /// 辐射度取自上一帧的 HDR（§9.2-P / §10.1 SSGI-CAL）。
    void SetRadianceHistory(GIRadianceHistory* radiance) { m_Radiance = radiance; }
    /// 白炉数值测试开关（帧图每帧给出）：白炉条件是「全白环境 + albedo = 1」，
    /// 本 pass 必须把自己也切到该条件，否则白炉判据只能靠短路、量纲错误永远抓不到。
    void SetFurnaceMode(bool on) { m_Furnace = on; }
    rhi::IRHISampler* GetOutputSampler() const { return m_Sampler.get(); }
    void PreBind(rhi::IRHICommandList* cmd) const { if (m_Ready) cmd->SetPipeline(m_PSO.get()); }

    float radius = 1.0f;
    int   sampleCount = 16;

private:
    void CreateOutputTex(u32 w, u32 h);
    /// 把「前帧 HDR 辐射度」绑到 binding 4；仅当组件代次变化（resize 重建）时才实际重绑
    void BindRadianceHistory();
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
    bool m_Furnace = false;                  // 白炉：本 pass 也要按 albedo=1 求值
    GIRadianceHistory* m_Radiance = nullptr; // 非拥有；前帧 HDR 辐射度（入射辐射度来源）
    u32 m_RadianceGeneration = 0xFFFFFFFFu;  // 已绑定的组件代次（见 BindRadianceHistory）
};

} // namespace he::render
