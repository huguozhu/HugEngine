#pragma once

#include "Subsystem/RenderSubsystem.h"
// 阶段 1 §15.1 第③段：本 Pass 改从快照取天空数据（不再遍历 World / 不再缓存组件指针）
#include "Threading/FrameSceneSnapshot.h"
#include "RHI/RHI.h"
#include "RHI/Shader.h"
#include <memory>

namespace he { class World; class SkyboxComponent; class PhysicalSkyComponent; }

namespace he::render {

// ============================================================================
// SkyboxPass — 天空盒渲染（全屏三角形，depth=Equal，无 VB/IB）
//
// 【阶段 1 §15.1 第③段】原先在 `Update(ctx)` 里遍历 World 找 `SkyboxComponent` /
// `PhysicalSkyComponent` 并**缓存组件指针** —— 那是渲染期读 ECS（换线程即竞争）。
// 现在只读 `SubsystemContext::snapshot` 里的 `skybox` / `physicalSky`（收集侧已按同一口径填好），
// 缓存的是**值 + RHI 资源指针**（cubemap/sampler），组件指针一个都不留。
// ============================================================================
class SkyboxPass : public IRenderSubsystem {
public:
    SkyboxPass()=default;
    ~SkyboxPass()override=default;

    bool Initialize(rhi::IRHIDevice* device,u32 width,u32 height)override;
    void Shutdown()override;
    void Update(const SubsystemContext& ctx)override;
    void Render(rhi::IRHICommandList* cmd)override;
    void Bind(rhi::IRHICommandList* cmd)const override{}
    void OnResize(u32 width,u32 height)override{}

    const char* GetName()const override{return"SkyboxPass";}
    bool IsReady()const override{return m_Ready;}
    bool IsEnabled()const override{return m_Enabled;}
    void SetEnabled(bool e)override{m_Enabled=e;}

    // 设置颜色附件 LoadOp（Clear=作为背景清屏，Load=叠加到已有内容，供 Deferred 管线在 Lighting 后合成）
    void SetColorLoadOp(rhi::LoadOp op);
    // 预设 PSO 到命令列表（供 RenderGraph 在 BeginOffscreenPass 前调用）
    void PreBind(rhi::IRHICommandList* cmd) const;

private:
    void CreatePSOs();

    rhi::ShaderBytecode m_VS,m_FS;
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_DescLayout=rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_DescSet=rhi::kInvalidSet;

    // 物理天空（Preetham 解析模型，优先级高于 Cubemap）
    rhi::ShaderBytecode m_PS_FS;
    std::unique_ptr<rhi::IRHIPipelineState> m_PS_PSO;

    // 天空盒 cubemap（值缓存：只用于"换了一张就更新描述符集"的判据；所有权在资源层）
    const rhi::IRHITexture* m_CachedCubemap = nullptr;
    rhi::IRHISampler*       m_CachedSampler = nullptr;
    float                   m_CachedSkyboxIntensity = 0.0f;
    // 物理天空参数（按值缓存，来自快照；不再是组件指针）
    SnapshotPhysicalSky m_PhysSky{};
    CameraData m_CachedCamera{};
    bool m_HasCamera=false;
    bool m_Ready=false;
    rhi::LoadOp m_ColorLoadOp = rhi::LoadOp::Clear;  // 颜色附件 LoadOp（Deferred 用 Load 叠加到 Lighting 结果）
};

} // namespace he::render
