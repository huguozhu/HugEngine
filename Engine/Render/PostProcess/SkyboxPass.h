#pragma once

#include "Subsystem/RenderSubsystem.h"
#include "RHI/RHI.h"
#include "RHI/Shader.h"
#include <memory>

namespace he { class World; class SkyboxComponent; }

namespace he::render {

// ============================================================================
// SkyboxPass — 天空盒渲染（全屏三角形，depth=Equal，无 VB/IB）
//
// 遍历 World 找到 SkyboxComponent → 绑定 Cubemap → 逆 ViewProj 采样
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

private:
    rhi::ShaderBytecode m_VS,m_FS;
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;
    rhi::DescriptorSetLayoutHandle m_DescLayout=rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_DescSet=rhi::kInvalidSet;

    const he::SkyboxComponent* m_CachedSkybox=nullptr;
    CameraData m_CachedCamera{};
    bool m_HasCamera=false;
    bool m_Ready=false;
};

} // namespace he::render
