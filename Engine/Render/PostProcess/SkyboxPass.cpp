#include "PostProcess/SkyboxPass.h"
#include "Skybox.vert.spv.h"
#include "Skybox.frag.spv.h"
#include "PhysicalSky.frag.spv.h"
#include "Scene/World.h"
#include "Scene/SkyboxComponent.h"
#include "Scene/PhysicalSkyComponent.h"
#include "Math/Math.h"
#include <glm/gtc/matrix_transform.hpp>
#include "Core/Log.h"
#include "Core/Assert.h"

namespace he::render {

bool SkyboxPass::Initialize(rhi::IRHIDevice* device,u32,u32){
    m_Device=device;HE_ASSERT(m_Device,"SkyboxPass: null device");

    m_VS.stage=rhi::ShaderStage::Vertex;m_VS.spirv=k_Skybox_vert_spv;m_VS.entryPoint="main";
    m_FS.stage=rhi::ShaderStage::Pixel;m_FS.spirv=k_Skybox_frag_spv;m_FS.entryPoint="main";
    m_PS_FS.stage=rhi::ShaderStage::Pixel;m_PS_FS.spirv=k_PhysicalSky_frag_spv;m_PS_FS.entryPoint="main";

    rhi::DescriptorSetLayoutDesc layout;layout.bindings={
        {10,rhi::DescriptorType::CombinedImageSampler,1,16},
    };
    m_DescLayout=device->CreateDescriptorSetLayout(layout);
    m_DescSet=device->AllocateDescriptorSet(m_DescLayout);

    CreatePSOs();

    m_Ready=true;HE_CORE_INFO("SkyboxPass init");return true;
}

void SkyboxPass::CreatePSOs(){
    // Cubemap 天空盒 PSO
    rhi::PushConstantRange pcr;pcr.stageMask=rhi::kStageMaskVertex|rhi::kStageMaskFragment;pcr.offset=0;pcr.size=96;
    rhi::PipelineStateDesc d;d.vertexShader=&m_VS;d.pixelShader=&m_FS;
    d.topology=rhi::PrimitiveTopology::TriangleList;
    d.depthTest=true;d.depthWrite=false;d.depthCompare=rhi::CompareFunc::Equal;
    d.depthFormat=rhi::Format::D32_FLOAT;
    d.colorAttachmentCount=1;d.colorFormats[0]=rhi::Format::RGBA16_FLOAT;
    d.colorLoadOp=m_ColorLoadOp;
    d.depthLoadOp=rhi::LoadOp::Load;  // 保留深度（Deferred 用独立 render pass，depth=Equal 需读 GBuffer 深度）
    d.pushConstantRanges={pcr};d.descriptorSetLayouts={m_DescLayout};d.debugName="Skybox";
    m_PSO=m_Device->CreatePipelineState(d);
    HE_ASSERT(m_PSO,"SkyboxPass: PSO failed");

    // 物理天空 PSO（Preetham 解析模型）
    // push constant 布局：invVP(64) + intensity(4) + pad(12) + sunDir(12) + turbidity/groundAlbedo/sunIntensity/pad(16) = 108，alignas(16) 对齐到 112
    rhi::PushConstantRange pcr2;pcr2.stageMask=rhi::kStageMaskVertex|rhi::kStageMaskFragment;pcr2.offset=0;pcr2.size=112;
    rhi::PipelineStateDesc d2;d2.vertexShader=&m_VS;d2.pixelShader=&m_PS_FS;
    d2.topology=rhi::PrimitiveTopology::TriangleList;
    d2.depthTest=true;d2.depthWrite=false;d2.depthCompare=rhi::CompareFunc::Equal;
    d2.depthFormat=rhi::Format::D32_FLOAT;
    d2.colorAttachmentCount=1;d2.colorFormats[0]=rhi::Format::RGBA16_FLOAT;
    d2.colorLoadOp=m_ColorLoadOp;
    d2.depthLoadOp=rhi::LoadOp::Load;  // 保留深度
    d2.pushConstantRanges={pcr2};d2.descriptorSetLayouts={m_DescLayout};d2.debugName="PhysicalSky";
    m_PS_PSO=m_Device->CreatePipelineState(d2);
    HE_ASSERT(m_PS_PSO,"SkyboxPass: PhysicalSky PSO failed");
}

void SkyboxPass::SetColorLoadOp(rhi::LoadOp op){
    if(op==m_ColorLoadOp)return;
    m_ColorLoadOp=op;
    if(m_Ready)CreatePSOs();
}

void SkyboxPass::PreBind(rhi::IRHICommandList* cmd) const {
    if(!m_Ready)return;
    // 物理天空优先于 Cubemap
    cmd->SetPipeline(m_CachedPhysSky ? m_PS_PSO.get() : m_PSO.get());
}

void SkyboxPass::Shutdown(){
    if(m_Device&&m_DescLayout!=rhi::kInvalidLayout)m_Device->DestroyDescriptorSetLayout(m_DescLayout);
    m_PSO.reset();
    m_PS_PSO.reset();
    m_CachedSkybox=nullptr;
    m_CachedPhysSky=nullptr;
    m_Device=nullptr;m_Ready=false;
}

void SkyboxPass::Update(const SubsystemContext& ctx){
    if(!m_Ready)return;

    // 缓存相机数据
    if(ctx.camera){m_CachedCamera=*ctx.camera;m_HasCamera=true;}else{m_HasCamera=false;}

    // 查找物理天空组件（优先级高于 Cubemap）
    if(!ctx.world)return;
    const he::PhysicalSkyComponent* foundSky=nullptr;
    ctx.world->ForEach<he::PhysicalSkyComponent>([&](he::Entity,he::PhysicalSkyComponent& ps){
        if(ps.enabled)foundSky=&ps;
    });
    m_CachedPhysSky=foundSky;

    // 查找启用且有效的 SkyboxComponent（物理天空不存在时回退）
    const he::SkyboxComponent* found=nullptr;
    ctx.world->ForEach<he::SkyboxComponent>([&](he::Entity,he::SkyboxComponent& sc){
        if(sc.enabled&&sc.GetCubemap())found=&sc;
    });
    if(!found){m_CachedSkybox=nullptr;return;}

    if(found!=m_CachedSkybox){
        m_CachedSkybox=found;
        m_Device->UpdateDescriptorSet(m_DescSet,10,
            rhi::DescriptorType::CombinedImageSampler,
            found->GetCubemap(),found->GetCubemapSampler());
    }
}

void SkyboxPass::Render(rhi::IRHICommandList* cmd){
    if(!m_Ready||!m_Enabled||(!m_CachedSkybox&&!m_CachedPhysSky)||!m_HasCamera)return;

    // 计算相机原点旋转视图的逆 ViewProj（去除平移影响，天空盒无限远）
    float4x4 viewRot=glm::lookAtRH(float3(0),m_CachedCamera.forward,m_CachedCamera.up);
    float4x4 invVP=glm::inverse(m_CachedCamera.GetProjMatrix()*viewRot);

    // 物理天空：解析 Preetham 模型（无纹理绑定，推入天空参数）
    if(m_CachedPhysSky){
        // 注意：Slang push constant 用 std430 布局，float3 对齐到 16 字节，
        // 故 intensity 后需补 3 个 float 的 padding，让 sunDir 落在 offset 80（与 shader 一致）
        struct alignas(16){float4x4 invVP;float intensity;float _pad0[3];float sunDir[3];float turbidity;float groundAlbedo;float sunIntensity;float _pad;}pc;
        pc.invVP=invVP;pc.intensity=m_CachedPhysSky->intensity;
        pc._pad0[0]=pc._pad0[1]=pc._pad0[2]=0.0f;
        pc.sunDir[0]=m_CachedPhysSky->sunDirection.x;
        pc.sunDir[1]=m_CachedPhysSky->sunDirection.y;
        pc.sunDir[2]=m_CachedPhysSky->sunDirection.z;
        pc.turbidity=m_CachedPhysSky->turbidity;
        pc.groundAlbedo=m_CachedPhysSky->groundAlbedo;
        pc.sunIntensity=m_CachedPhysSky->sunIntensity;
        pc._pad=0.0f;
        cmd->SetPipeline(m_PS_PSO.get());
        cmd->SetPushConstants(0,sizeof(pc),&pc);
        cmd->Draw(3);
        return;
    }

    struct alignas(16){float4x4 invVP;float intensity;float _pad[7];}pc;
    pc.invVP=invVP;pc.intensity=m_CachedSkybox->intensity;

    cmd->SetPipeline(m_PSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame,m_DescSet);
    cmd->SetPushConstants(0,96,&pc);
    cmd->Draw(3);
}

} // namespace he::render
