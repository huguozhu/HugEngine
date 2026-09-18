#include "GI/RSMIndirect.h"
#include "Core/Log.h"
#include <cstring>

#include "SSAO.vert.spv.h"          // 全屏三角顶点着色器（与 SSGI/AO 共用）
#include "RSM_Indirect.frag.spv.h"

namespace he::render {

bool RSMIndirect::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width  = width;
    m_Height = height;

    // ---- 参数 UBO：光源 VP + (lightCount, 光源类型, 阴影强度) ----
    rhi::BufferDesc ubDesc;
    ubDesc.size      = sizeof(float4x4) + sizeof(float4);
    ubDesc.usage     = rhi::BufferUsage::Uniform;
    ubDesc.cpuAccess = true;
    m_ParamsUBO = device->CreateBuffer(ubDesc);

    // ---- 描述符集 ----
    rhi::DescriptorSetLayoutDesc layoutDesc;
    layoutDesc.bindings = {
        {kBindDepth,    rhi::DescriptorType::CombinedImageSampler, 1, 16},
        {kBindWorldPos, rhi::DescriptorType::CombinedImageSampler, 1, 16},
        {kBindNormal,   rhi::DescriptorType::CombinedImageSampler, 1, 16},
        {kBindRSMPos,   rhi::DescriptorType::CombinedImageSampler, 1, 16},
        {kBindRSMFlux,  rhi::DescriptorType::CombinedImageSampler, 1, 16},
        {kBindParams,   rhi::DescriptorType::UniformBuffer,        1, 16},
    };
    m_Layout = device->CreateDescriptorSetLayout(layoutDesc);
    m_Set    = device->AllocateDescriptorSet(m_Layout);
    device->UpdateDescriptorSet(m_Set, kBindParams, rhi::DescriptorType::UniformBuffer, m_ParamsUBO.get());

    // ---- 采样器 ----
    rhi::SamplerDesc psd;
    psd.minFilter = psd.magFilter = rhi::FilterMode::Nearest;
    psd.addressU  = psd.addressV  = rhi::AddressMode::ClampToEdge;
    m_PointSampler = device->CreateSampler(psd);

    rhi::SamplerDesc lsd;
    lsd.minFilter = lsd.magFilter = rhi::FilterMode::Linear;
    lsd.addressU  = lsd.addressV  = rhi::AddressMode::ClampToEdge;
    m_LinearSampler = device->CreateSampler(lsd);

    // ---- 管线：全屏三角 → 半分辨率 RSM 间接光 ----
    rhi::ShaderBytecode vs, fs;
    vs.stage      = rhi::ShaderStage::Vertex;
    vs.spirv      = k_SSAO_vert_spv;
    vs.entryPoint = "vertexMain";
    fs.stage      = rhi::ShaderStage::Pixel;
    fs.spirv      = k_RSM_Indirect_frag_spv;
    fs.entryPoint = "fragmentMain";

    rhi::PipelineStateDesc psoDesc;
    psoDesc.vertexShader         = &vs;
    psoDesc.pixelShader          = &fs;
    psoDesc.topology             = rhi::PrimitiveTopology::TriangleList;
    psoDesc.depthTest            = false;
    psoDesc.depthWrite           = false;
    psoDesc.depthFormat          = rhi::Format::Unknown;
    psoDesc.colorAttachmentCount = 1;
    psoDesc.colorFormats[0]      = rhi::Format::RGBA16_FLOAT;
    psoDesc.descriptorSetLayouts = {m_Layout};
    psoDesc.debugName            = "RSM_Indirect";
    m_PSO = device->CreatePipelineState(psoDesc);

    CreateOutput(width, height);
    m_Ready = true;
    HE_CORE_INFO("RSM 间接光 pass initialized ({}x{} -> {}x{})",
                 width, height, GetOutputWidth(), GetOutputHeight());
    return true;
}

void RSMIndirect::Shutdown() {
    m_PSO.reset();
    m_Output.reset();
    m_ParamsUBO.reset();
    m_PointSampler.reset();
    m_LinearSampler.reset();
    if (m_Device && m_Layout != rhi::kInvalidLayout) {
        m_Device->DestroyDescriptorSetLayout(m_Layout);
        m_Layout = rhi::kInvalidLayout;
        m_Set    = rhi::kInvalidSet;
    }
    m_Device = nullptr;
    m_Ready  = false;
}

void RSMIndirect::OnResize(u32 width, u32 height) {
    if (width == m_Width && height == m_Height) return;
    m_Width  = width;
    m_Height = height;
    if (m_Ready) CreateOutput(width, height);
}

void RSMIndirect::CreateOutput(u32 width, u32 height) {
    const u32 w = (width  + kDownscale - 1u) / kDownscale;
    const u32 h = (height + kDownscale - 1u) / kDownscale;
    rhi::TextureDesc td;
    td.format      = rhi::Format::RGBA16_FLOAT;
    td.width       = w ? w : 1u;
    td.height      = h ? h : 1u;
    td.mipLevels   = 1;
    td.arrayLayers = 1;
    td.usage       = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::RenderTarget;
    m_Output = m_Device->CreateTexture(td);
}

void RSMIndirect::SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* worldPos, rhi::IRHITexture* normal) {
    m_Depth    = depth;
    m_WorldPos = worldPos;
    m_Normal   = normal;
    // 纹理每帧可能变化，逐个重绑（代价是一条描述符写，与 SSGI 的做法一致）
    if (m_Device) {
        if (m_Depth)    m_Device->UpdateDescriptorSet(m_Set, kBindDepth,    rhi::DescriptorType::CombinedImageSampler, m_Depth,    m_PointSampler.get());
        if (m_WorldPos) m_Device->UpdateDescriptorSet(m_Set, kBindWorldPos, rhi::DescriptorType::CombinedImageSampler, m_WorldPos, m_PointSampler.get());
        if (m_Normal)   m_Device->UpdateDescriptorSet(m_Set, kBindNormal,   rhi::DescriptorType::CombinedImageSampler, m_Normal,   m_PointSampler.get());
    }
}

void RSMIndirect::SetRSM(rhi::IRHITexture* positionMap, rhi::IRHITexture* fluxMap,
                         const float4x4& lightViewProj, float shadowType, float shadowStrength,
                         u32 lightCount) {
    m_RSMPos  = positionMap;
    m_RSMFlux = fluxMap;
    if (!m_Device) return;
    if (m_RSMPos)  m_Device->UpdateDescriptorSet(m_Set, kBindRSMPos,  rhi::DescriptorType::CombinedImageSampler, m_RSMPos,  m_LinearSampler.get());
    if (m_RSMFlux) m_Device->UpdateDescriptorSet(m_Set, kBindRSMFlux, rhi::DescriptorType::CombinedImageSampler, m_RSMFlux, m_LinearSampler.get());

    struct alignas(16) {
        float4x4 lightViewProj;
        float4   params;   // x=lightCount, y=光源类型, z=阴影强度
    } ub;
    ub.lightViewProj = lightViewProj;
    ub.params = float4(float(lightCount), shadowType, shadowStrength, 0.0f);
    void* mapped = m_ParamsUBO->Map();
    if (mapped) {
        memcpy(mapped, &ub, sizeof(ub));
        m_ParamsUBO->Unmap();
    }
}

void RSMIndirect::PreBind(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_PSO || !m_Output) return;
    // 见头文件：绑管线必须早于 BeginOffscreenPass
    cmd->SetPipeline(m_PSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Set);
    // 视口用输出纹理实际尺寸（半分辨率），与渲染目标一致
    const u32 ow = m_Output->GetWidth();
    const u32 oh = m_Output->GetHeight();
    cmd->SetViewport({0, (float)oh, (float)ow, -(float)oh, 0, 1});
    cmd->SetScissor({0, 0, ow, oh});
}

void RSMIndirect::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_PSO || !m_Output) return;
    if (!m_Depth || !m_WorldPos || !m_Normal || !m_RSMPos || !m_RSMFlux) return;
    cmd->Draw(3);   // 全屏三角（顶点着色器覆盖整个视口 ⇒ 每个像素都被写）
}

} // namespace he::render
