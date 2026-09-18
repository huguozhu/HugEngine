// GI/GI_SSR.cpp — 屏幕空间反射
#include "GI/GI_SSR.h"
#include "Core/Log.h"
#include "SSAO.vert.spv.h"
#include "SSR.frag.spv.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>

namespace he::render {

// SSR 描述符集绑定号（与 SSR.frag 的 vk::binding 一致）
static constexpr u32 kSSRBindDepth  = 0;   // 深度
static constexpr u32 kSSRBindNormal = 1;   // 法线
static constexpr u32 kSSRBindAlbedo = 2;   // 反照率
static constexpr u32 kSSRBindParams = 3;   // 矩阵 UBO（invProj + proj）
static constexpr u32 kSSRBindHiZ    = 4;   // Hi-Z 深度金字塔

// SSR 矩阵 UBO 布局（与 SSR.frag 的 cbuffer SSRMatrices 逐字段一致）
struct SSRMatrices {
    float4x4 invProj;   // clip → view：重建 view-space 位置
    float4x4 proj;      // view → clip：把射线采样点投影到屏幕（此前缺失，用逆矩阵顶替）
};

bool GI_SSR::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width = width;
    m_Height = height;
    m_Settings.enabled = false;
    m_Settings.intensity = 1.0f;
    m_Settings.mode = GIMode::SSGI;

    rhi::DescriptorSetLayoutDesc l;
    l.bindings = {
        {kSSRBindDepth,  rhi::DescriptorType::CombinedImageSampler, 1, 16},
        {kSSRBindNormal, rhi::DescriptorType::CombinedImageSampler, 1, 16},
        {kSSRBindAlbedo, rhi::DescriptorType::CombinedImageSampler, 1, 16},
        {kSSRBindParams, rhi::DescriptorType::UniformBuffer, 1, 16},
        {kSSRBindHiZ,    rhi::DescriptorType::CombinedImageSampler, 1, 16},
    };
    m_DescLayout = device->CreateDescriptorSetLayout(l);
    m_DescSet = device->AllocateDescriptorSet(m_DescLayout);

    // 矩阵 UBO（binding 3）：invProj（clip→view）+ proj（view→clip）
    // 两个目的各用一个矩阵、互不顶替 —— 这正是 §9.2-B 的修复点。
    rhi::BufferDesc ubDesc;
    ubDesc.size      = sizeof(SSRMatrices);
    ubDesc.usage     = rhi::BufferUsage::Uniform;
    ubDesc.cpuAccess = true;
    m_UniformBuffer  = device->CreateBuffer(ubDesc);
    device->UpdateDescriptorSet(m_DescSet, kSSRBindParams,
        rhi::DescriptorType::UniformBuffer, m_UniformBuffer.get());

    rhi::ShaderBytecode vs, fs;
    vs.stage = rhi::ShaderStage::Vertex;
    vs.spirv = k_SSAO_vert_spv;
    vs.entryPoint = "vertexMain";
    fs.stage = rhi::ShaderStage::Pixel;
    fs.spirv = k_SSR_frag_spv;
    fs.entryPoint = "fragmentMain";
    rhi::PipelineStateDesc d;
    d.vertexShader = &vs;
    d.pixelShader = &fs;
    d.topology = rhi::PrimitiveTopology::TriangleList;
    d.depthTest = false;
    d.depthWrite = false;
    d.depthFormat = rhi::Format::Unknown;
    d.colorAttachmentCount = 1;
    d.colorFormats[0] = rhi::Format::RGBA16_FLOAT;
    d.descriptorSetLayouts = {m_DescLayout};
    d.debugName = "SSR";
    m_PSO = device->CreatePipelineState(d);

    rhi::SamplerDesc sd;
    sd.minFilter = sd.magFilter = rhi::FilterMode::Nearest;
    sd.addressU = sd.addressV = rhi::AddressMode::ClampToEdge;
    m_PointSampler = device->CreateSampler(sd);
    CreateOutputTex(halfResW(width), halfResH(height));
    m_Ready = true;
    HE_CORE_INFO("GI_SSR initialized");
    return true;
}

void GI_SSR::Shutdown() {
    m_PSO.reset();
    m_UniformBuffer.reset();
    if (m_Device && m_DescLayout != rhi::kInvalidLayout) {
        m_Device->DestroyDescriptorSetLayout(m_DescLayout);
    }
    m_Output.reset();
    m_Sampler.reset();
    m_PointSampler.reset();
    m_Device = nullptr;
    m_Ready = false;
}

void GI_SSR::OnResize(u32 w, u32 h) {
    m_Width = w;
    m_Height = h;
    CreateOutputTex(halfResW(w), halfResH(h));
}

void GI_SSR::SyncOutputSize() {
    if (!m_Device || !m_Ready) return;
    const u32 w = halfResW(m_Width), h = halfResH(m_Height);
    if (!m_Output || m_Output->GetWidth() != w || m_Output->GetHeight() != h) {
        CreateOutputTex(w, h);
    }
}

void GI_SSR::CreateOutputTex(u32 w, u32 h) {
    rhi::TextureDesc td;
    td.format = rhi::Format::RGBA16_FLOAT;
    td.width = w;
    td.height = h;
    td.mipLevels = 1;
    td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_Output = m_Device->CreateTexture(td);
    rhi::SamplerDesc sd;
    sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
    sd.addressU = sd.addressV = rhi::AddressMode::ClampToEdge;
    m_Sampler = m_Device->CreateSampler(sd);
}

void GI_SSR::SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal, rhi::IRHITexture* albedo) {
    m_Depth = depth;
    m_Normal = normal;
    m_Albedo = albedo;
    // 每帧纹理可能变化，动态更新描述符集绑定
    if (m_Depth) {
        m_Device->UpdateDescriptorSet(m_DescSet, kSSRBindDepth, rhi::DescriptorType::CombinedImageSampler, m_Depth, m_PointSampler.get());
    }
    if (m_Normal) {
        m_Device->UpdateDescriptorSet(m_DescSet, kSSRBindNormal, rhi::DescriptorType::CombinedImageSampler, m_Normal, m_PointSampler.get());
    }
    if (m_Albedo) {
        m_Device->UpdateDescriptorSet(m_DescSet, kSSRBindAlbedo, rhi::DescriptorType::CombinedImageSampler, m_Albedo, m_PointSampler.get());
    }
}

void GI_SSR::SetHiZ(rhi::IRHITexture* hiZ, rhi::IRHISampler* sampler) {
    m_HiZTex = hiZ;
    m_HiZSampler = sampler;
    if (m_Device && hiZ && sampler) {
        m_Device->UpdateDescriptorSet(m_DescSet, kSSRBindHiZ, rhi::DescriptorType::CombinedImageSampler,
            hiZ, sampler);
    }
}

void GI_SSR::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_Settings.enabled || !m_Depth || !m_Normal || !m_Albedo) {
        return;
    }
    cmd->SetPipeline(m_PSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_DescSet);
    u32 ow = m_Output->GetWidth();
    u32 oh = m_Output->GetHeight();
    cmd->SetViewport({0, (float)oh, (float)ow, -(float)oh, 0, 1});
    cmd->SetScissor({0, 0, ow, oh});
    // ── 矩阵上传（UBO binding 3）：**正/逆投影分开传** ──
    // 此前只传了 inverse(proj)，而 shader 还拿它做 view→clip 投影 —— 于是
    // Hi-Z 层次 march 与线性 march **两条路径都在错误的屏幕位置采样深度**（§9.2-B）。
    {
        const float aspect = float(m_Width) / float(m_Height);
        SSRMatrices mats;
        mats.proj    = glm::perspectiveRH_ZO(glm::radians(kDefaultFOV), aspect,
                                             kDefaultNearPlane, kDefaultFarPlane);
        mats.invProj = glm::inverse(mats.proj);
        void* mapped = m_UniformBuffer->Map();
        if (mapped) {
            std::memcpy(mapped, &mats, sizeof(mats));
            m_UniformBuffer->Unmap();
        }
    }

    // push constant 只剩标量（32B，从原先的 96B 缩小）
    // —— 矩阵不再占用这个 128B 的稀缺额度，后续若要再传相机数据也不会立刻撞上限
    struct {
        float4 p;          // x=maxSteps, y=stepSize, z=maxDistance, w=thickness
        float  useHiZ;     // 1=Hi-Z 层次 march，0=线性 march
        float  _pad[3];
    } pc;
    pc.p = float4(maxSteps, stepSize, maxDistance, thickness);
    pc.useHiZ = (m_HiZTex != nullptr) ? 1.0f : 0.0f;   // Hi-Z 金字塔可用时启用层次追踪
    pc._pad[0] = pc._pad[1] = pc._pad[2] = 0.0f;
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    cmd->Draw(3);
}

} // namespace he::render
