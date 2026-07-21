// PostProcess/SSAO.cpp — SSAO 实现
#include "PostProcess/SSAO.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "SSAO.vert.spv.h"
#include "SSAO.frag.spv.h"
#include "SSAO_Blur.vert.spv.h"
#include "SSAO_Blur.frag.spv.h"
#include <glm/gtc/matrix_transform.hpp>
#include <random>
#include <cstring>

namespace he::render {

void SSAO::GenerateKernel() {
    m_Kernel.resize(kKernelSize);
    std::uniform_real_distribution<float> rnd(0.0f, 1.0f);
    std::default_random_engine gen(42);
    for (int i = 0; i < kKernelSize; ++i) {
        // 半球内均匀分布，偏向中心
        float3 sample(rnd(gen)*2-1, rnd(gen)*2-1, rnd(gen));
        sample = glm::normalize(sample);
        sample *= rnd(gen);
        // 缩放使靠近中心更多
        float scale = float(i) / float(kKernelSize);
        scale = glm::mix(0.1f, 1.0f, scale * scale);
        m_Kernel[i] = float4(sample * scale, 0);
    }
}

void SSAO::GenerateNoise(u32 size) {
    std::uniform_real_distribution<float> rnd(-1.0f, 1.0f);
    std::default_random_engine gen(123);
    std::vector<float4> noise(size * size);
    for (u32 i = 0; i < size * size; ++i)
        noise[i] = float4(rnd(gen), rnd(gen), 0, 0);  // 2D 旋转向量
    rhi::TextureDesc td; td.format=rhi::Format::RGBA16_FLOAT;
    td.width=size; td.height=size; td.mipLevels=1;
    td.usage=rhi::TextureUsage::ShaderResource; td.initialData=noise.data();
    m_NoiseTex = m_Device->CreateTexture(td);
}

void SSAO::CreateAOTexture(u32 w, u32 h) {
    rhi::TextureDesc td; td.format=rhi::Format::R16_FLOAT;
    td.width=w; td.height=h; td.mipLevels=1;
    td.usage=rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_AOTexture = m_Device->CreateTexture(td);
    rhi::SamplerDesc sd; sd.minFilter=sd.magFilter=rhi::FilterMode::Linear;
    sd.addressU=sd.addressV=rhi::AddressMode::ClampToEdge;
    m_AOSampler = m_Device->CreateSampler(sd);
}

void SSAO::CreateBlurTexture(u32 w, u32 h) {
    rhi::TextureDesc td; td.format=rhi::Format::R16_FLOAT;
    td.width=w; td.height=h; td.mipLevels=1;
    td.usage=rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_BlurTexture = m_Device->CreateTexture(td);
}

bool SSAO::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device; m_Width = width; m_Height = height;

    GenerateKernel();
    GenerateNoise(4);

    // SSAO PSO
    {
        rhi::DescriptorSetLayoutDesc l;
        l.bindings = {{0,rhi::DescriptorType::CombinedImageSampler,1,16},  // Depth
                      {1,rhi::DescriptorType::CombinedImageSampler,1,16},  // Normal
                      {2,rhi::DescriptorType::CombinedImageSampler,1,16},  // Noise
                      {3,rhi::DescriptorType::UniformBuffer,1,16}};         // Params
        m_SSAOLayout = device->CreateDescriptorSetLayout(l);
        m_SSAOSet    = device->AllocateDescriptorSet(m_SSAOLayout);

        // 创建 SSAO 参数 UBO（kernel[64] + params + proj = 1104 bytes）
        rhi::BufferDesc ubDesc;
        ubDesc.size  = 64 * sizeof(float4) + sizeof(float4) + sizeof(float4x4) * 2;  // kernel + params + invProj + proj
        ubDesc.usage = rhi::BufferUsage::Uniform;
        ubDesc.cpuAccess = true;
        m_ParamUBO = device->CreateBuffer(ubDesc);
        device->UpdateDescriptorSet(m_SSAOSet, 3, rhi::DescriptorType::UniformBuffer, m_ParamUBO.get());

        rhi::ShaderBytecode vs,fs;
        vs.stage=rhi::ShaderStage::Vertex; vs.spirv=k_SSAO_vert_spv; vs.entryPoint="vertexMain";
        fs.stage=rhi::ShaderStage::Pixel;  fs.spirv=k_SSAO_frag_spv; fs.entryPoint="fragmentMain";
        rhi::PipelineStateDesc d; d.vertexShader=&vs; d.pixelShader=&fs;
        d.topology=rhi::PrimitiveTopology::TriangleList;
        d.depthTest=false; d.depthWrite=false; d.depthFormat=rhi::Format::Unknown;
        d.colorAttachmentCount=1; d.colorFormats[0]=rhi::Format::R16_FLOAT;
        d.descriptorSetLayouts={m_SSAOLayout}; d.debugName="SSAO";
        m_SSAO_PSO = device->CreatePipelineState(d);
    }

    // Blur PSO
    {
        rhi::DescriptorSetLayoutDesc l;
        l.bindings = {{0,rhi::DescriptorType::CombinedImageSampler,1,16}};
        m_BlurLayout = device->CreateDescriptorSetLayout(l);
        m_BlurSet    = device->AllocateDescriptorSet(m_BlurLayout);

        rhi::ShaderBytecode vs,fs;
        vs.stage=rhi::ShaderStage::Vertex; vs.spirv=k_SSAO_Blur_vert_spv; vs.entryPoint="vertexMain";
        fs.stage=rhi::ShaderStage::Pixel;  fs.spirv=k_SSAO_Blur_frag_spv; fs.entryPoint="fragmentMain";
        rhi::PipelineStateDesc d; d.vertexShader=&vs; d.pixelShader=&fs;
        d.topology=rhi::PrimitiveTopology::TriangleList;
        d.depthTest=false; d.depthWrite=false; d.depthFormat=rhi::Format::Unknown;
        d.colorAttachmentCount=1; d.colorFormats[0]=rhi::Format::R16_FLOAT;
        d.descriptorSetLayouts={m_BlurLayout}; d.debugName="SSAO_Blur";
        m_Blur_PSO = device->CreatePipelineState(d);
    }

    // 输出纹理 + 采样器
    rhi::SamplerDesc ptSamp; ptSamp.minFilter=ptSamp.magFilter=rhi::FilterMode::Nearest;
    ptSamp.addressU=ptSamp.addressV=rhi::AddressMode::ClampToEdge;
    m_PointSampler = device->CreateSampler(ptSamp);

    CreateAOTexture(width, height);
    CreateBlurTexture(width, height);

    m_Ready = true;
    HE_CORE_INFO("SSAO initialized ({}×{})", width, height);
    return true;
}

void SSAO::Shutdown() {
    if (m_Device && m_SSAOLayout!=rhi::kInvalidLayout) m_Device->DestroyDescriptorSetLayout(m_SSAOLayout);
    if (m_Device && m_BlurLayout!=rhi::kInvalidLayout) m_Device->DestroyDescriptorSetLayout(m_BlurLayout);
    m_SSAO_PSO.reset(); m_Blur_PSO.reset();
    m_AOTexture.reset(); m_BlurTexture.reset(); m_AOSampler.reset(); m_PointSampler.reset(); m_NoiseTex.reset();
    m_ParamUBO.reset();
    m_Device = nullptr; m_Ready = false;
}

void SSAO::OnResize(u32 w, u32 h) { m_Width=w; m_Height=h; CreateAOTexture(w,h); CreateBlurTexture(w,h); }

void SSAO::SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal) {
    m_DepthTex = depth; m_NormalTex = normal;
    if (m_DepthTex) m_Device->UpdateDescriptorSet(m_SSAOSet,0,rhi::DescriptorType::CombinedImageSampler,m_DepthTex,m_PointSampler.get());
    if (m_NormalTex) m_Device->UpdateDescriptorSet(m_SSAOSet,1,rhi::DescriptorType::CombinedImageSampler,m_NormalTex,m_PointSampler.get());
    m_Device->UpdateDescriptorSet(m_SSAOSet,2,rhi::DescriptorType::CombinedImageSampler,m_NoiseTex.get(),m_PointSampler.get());
}

void SSAO::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_DepthTex || !m_NormalTex || !enabled) return;

    // --- SSAO Pass ---
    cmd->SetPipeline(m_SSAO_PSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_SSAOSet);
    cmd->SetViewport({0,(float)m_Height,(float)m_Width,-(float)m_Height,0,1});
    cmd->SetScissor({0,0,m_Width,m_Height});

    // 上传 SSAO 参数到 Uniform Buffer（kernel[64] + params + proj）
    // 对齐 shader 中 SSAOParams cbuffer 布局
    {
        u8* dst = static_cast<u8*>(m_ParamUBO->Map());
        if (dst) {
            // kernel[64] — 半球采样方向（view-space）
            memcpy(dst, m_Kernel.data(), 64 * sizeof(float4));
            dst += 64 * sizeof(float4);
            // params — x=radius, y=bias, z=intensity, w=sampleCount
            float4 p(radius, bias, intensity, float(sampleCount));
            memcpy(dst, &p, sizeof(float4));
            dst += sizeof(float4);
            // u_InvProj: 逆投影矩阵（clip→view，用于从深度重建 view-space 位置）
            float a=float(m_Width)/float(m_Height);
            float4x4 proj = glm::perspectiveRH_ZO(glm::radians(kDefaultFOV), a, kDefaultNearPlane, kDefaultFarPlane);
            float4x4 projInv = glm::inverse(proj);
            memcpy(dst, &projInv, sizeof(float4x4));
            dst += sizeof(float4x4);
            // u_Proj: 正投影矩阵（view→clip，用于将采样点投影到屏幕）
            memcpy(dst, &proj, sizeof(float4x4));
            m_ParamUBO->Unmap();
        }
    }
    cmd->Draw(3);

    // --- Blur Pass ---
    cmd->SetPipeline(m_Blur_PSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_BlurSet);
    m_Device->UpdateDescriptorSet(m_BlurSet,0,rhi::DescriptorType::CombinedImageSampler,m_AOTexture.get(),m_AOSampler.get());

    struct { float2 ts; float _pad[2]; } bpc;
    bpc.ts = float2(1.0f/m_Width, 1.0f/m_Height);
    cmd->SetPushConstants(0, sizeof(bpc), &bpc);
    cmd->Draw(3);
}

} // namespace he::render
