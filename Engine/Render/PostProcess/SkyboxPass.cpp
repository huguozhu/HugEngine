#include "PostProcess/SkyboxPass.h"
#include "Skybox.vert.spv.h"
#include "Skybox.frag.spv.h"
#include "Scene/World.h"
#include "Scene/SkyboxComponent.h"
#include "Math/Math.h"
#include <glm/gtc/matrix_transform.hpp>
#include "Core/Log.h"
#include "Core/Assert.h"

namespace he::render {

bool SkyboxPass::Initialize(rhi::IRHIDevice* device,u32,u32){
    m_Device=device;HE_ASSERT(m_Device,"SkyboxPass: null device");

    m_VS.stage=rhi::ShaderStage::Vertex;m_VS.spirv=k_Skybox_vert_spv;m_VS.entryPoint="main";
    m_FS.stage=rhi::ShaderStage::Pixel;m_FS.spirv=k_Skybox_frag_spv;m_FS.entryPoint="main";

    rhi::DescriptorSetLayoutDesc layout;layout.bindings={
        {10,rhi::DescriptorType::CombinedImageSampler,1,16},
    };
    m_DescLayout=device->CreateDescriptorSetLayout(layout);
    m_DescSet=device->AllocateDescriptorSet(m_DescLayout);

    rhi::PushConstantRange pcr;pcr.stageMask=1|16;pcr.offset=0;pcr.size=96;
    rhi::PipelineStateDesc d;d.vertexShader=&m_VS;d.pixelShader=&m_FS;
    d.topology=rhi::PrimitiveTopology::TriangleList;
    d.depthTest=true;d.depthWrite=false;d.depthCompare=rhi::CompareFunc::Equal;
    d.depthFormat=rhi::Format::D32_FLOAT;
    d.colorAttachmentCount=1;d.colorFormats[0]=rhi::Format::RGBA16_FLOAT;
    d.pushConstantRanges={pcr};d.descriptorSetLayouts={m_DescLayout};d.debugName="Skybox";
    m_PSO=device->CreatePipelineState(d);
    HE_ASSERT(m_PSO,"SkyboxPass: PSO failed");

    m_Ready=true;HE_CORE_INFO("SkyboxPass init");return true;
}

void SkyboxPass::Shutdown(){
    if(m_Device&&m_DescLayout!=rhi::kInvalidLayout)m_Device->DestroyDescriptorSetLayout(m_DescLayout);
    m_PSO.reset();m_Device=nullptr;m_Ready=false;
}

void SkyboxPass::Update(const SubsystemContext& ctx){
    if(!m_Ready)return;

    // 缓存相机数据
    if(ctx.camera){m_CachedCamera=*ctx.camera;m_HasCamera=true;}else{m_HasCamera=false;}

    // 查找启用且有效的 SkyboxComponent
    if(!ctx.world)return;
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
    if(!m_Ready||!m_Enabled||!m_CachedSkybox||!m_HasCamera)return;

    // 计算相机原点旋转视图的逆 ViewProj（去除平移影响，天空盒无限远）
    float4x4 viewRot=glm::lookAtRH(float3(0),m_CachedCamera.forward,m_CachedCamera.up);
    float4x4 invVP=glm::inverse(m_CachedCamera.GetProjMatrix()*viewRot);

    struct alignas(16){float4x4 invVP;float intensity;float _pad[7];}pc;
    pc.invVP=invVP;pc.intensity=m_CachedSkybox->intensity;

    cmd->SetPipeline(m_PSO.get());
    cmd->BindDescriptorSet(0,m_DescSet);
    cmd->SetPushConstants(0,96,&pc);
    cmd->Draw(3);
}

} // namespace he::render
