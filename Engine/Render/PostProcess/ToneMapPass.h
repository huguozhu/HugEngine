#pragma once

#include "Subsystem/RenderSubsystem.h"
#include "RHI/RHI.h"
#include "RHI/Shader.h"
#include <memory>

namespace he::render {

// ============================================================================
// ToneMapPass — ACES ToneMap 后处理（全屏三角形，无需 VB/IB）
//
// 读取 HDR 离屏渲染目标 → ACES ToneMap → Linear→sRGB → 输出到当前 RenderPass
// ============================================================================
class ToneMapPass : public IRenderSubsystem {
public:
    ToneMapPass()=default;
    ~ToneMapPass()override=default;

    bool Initialize(rhi::IRHIDevice* device,u32 width,u32 height)override;
    void Shutdown()override;
    void Update(const SubsystemContext&)override{}  // 无需场景数据
    void Render(rhi::IRHICommandList* cmd)override;
    void Bind(rhi::IRHICommandList* cmd)const override{}
    void OnResize(u32 width,u32 height)override;

    const char* GetName()const override{return"ToneMapPass";}
    bool IsReady()const override{return m_Ready;}
    bool IsEnabled()const override{return m_Enabled;}
    void SetEnabled(bool e)override{m_Enabled=e;}

    // 每帧设置 HDR 输入纹理
    void SetInput(rhi::IRHITexture* hdrTarget,rhi::IRHISampler* sampler);
    // 预设 PSO 为下一个 RenderPass 的初始管线
    void PreBind(rhi::IRHICommandList* cmd){if(m_Ready)cmd->SetPipeline(m_PSO.get());}

private:
    rhi::ShaderBytecode m_VS,m_FS;
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_DescLayout=rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_DescSet=rhi::kInvalidSet;
    rhi::IRHITexture* m_HDRTarget=nullptr;
    rhi::IRHISampler* m_HDRSampler=nullptr;
    u32 m_Width=0,m_Height=0;
    bool m_Ready=false;
};

} // namespace he::render
