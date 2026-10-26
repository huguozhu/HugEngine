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

    // ---- 参数 UBO：光源 VP + (lightCount, 光源类型, 阴影强度, VPL 采样缩放) ----
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
        {kBindRSMNrm,   rhi::DescriptorType::CombinedImageSampler, 1, 16},
        {kBindParams,   rhi::DescriptorType::UniformBuffer,        1, 16},
        {kBindRadiance, rhi::DescriptorType::CombinedImageSampler, 1, 16},
    };
    m_Layout = device->CreateDescriptorSetLayout(layoutDesc);
    m_Set    = device->AllocateDescriptorSet(m_Layout);
    device->UpdateDescriptorSet(m_Set, kBindParams, rhi::DescriptorType::UniformBuffer, m_ParamsUBO.get());

    // ---- 采样器 ----
    rhi::SamplerDesc psd;
    psd.minFilter = psd.magFilter = rhi::FilterMode::Nearest;
    psd.addressU  = psd.addressV  = rhi::AddressMode::ClampToEdge;
    m_PointSampler = device->CreateSamplerHandle(psd);

    rhi::SamplerDesc lsd;
    lsd.minFilter = lsd.magFilter = rhi::FilterMode::Linear;
    lsd.addressU  = lsd.addressV  = rhi::AddressMode::ClampToEdge;
    m_LinearSampler = device->CreateSamplerHandle(lsd);

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
    m_PSO = device->CreatePipelineStateHandle(psoDesc);

    CreateOutput(width, height);
    m_Ready = true;
    HE_CORE_INFO("RSM 间接光 pass initialized ({}x{} -> {}x{})",
                 width, height, GetOutputWidth(), GetOutputHeight());
    return true;
}

void RSMIndirect::Shutdown() {
    // 【空保护】m_Device 可能为空（Shutdown 被调用两次，或 Initialize 失败后进入清理路径），
    // 直接解引用会崩溃（实测：02.Cube 退出时连续崩在多个 Shutdown 里）。
    if (!m_Device) return;

    m_Device->DestroyPipelineStateHandle(m_PSO); m_PSO = {};
    m_Device->DestroyTextureHandle(m_Output); m_Output = {};
    m_ParamsUBO.reset();
    m_Device->DestroySamplerHandle(m_PointSampler); m_PointSampler = {};
    m_Device->DestroySamplerHandle(m_LinearSampler); m_LinearSampler = {};
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
    m_Output = m_Device->CreateTextureHandle(td);
}

void RSMIndirect::SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* worldPos, rhi::IRHITexture* normal) {
    m_Depth    = depth;
    m_WorldPos = worldPos;
    m_Normal   = normal;
    // 纹理每帧可能变化，逐个重绑（代价是一条描述符写，与 SSGI 的做法一致）
    if (m_Device) {
        if (m_Depth)    m_Device->UpdateDescriptorSet(m_Set, kBindDepth,    rhi::DescriptorType::CombinedImageSampler, m_Depth,    ResS(m_PointSampler));
        if (m_WorldPos) m_Device->UpdateDescriptorSet(m_Set, kBindWorldPos, rhi::DescriptorType::CombinedImageSampler, m_WorldPos, ResS(m_PointSampler));
        if (m_Normal)   m_Device->UpdateDescriptorSet(m_Set, kBindNormal,   rhi::DescriptorType::CombinedImageSampler, m_Normal,   ResS(m_PointSampler));
    }
}

void RSMIndirect::SetRSM(rhi::IRHITexture* positionMap, rhi::IRHITexture* normalMap,
                         rhi::IRHITexture* radianceMap,
                         const float4x4& lightViewProj, float vplScale,
                         float shadowType, float shadowStrength, u32 lightCount) {
    m_RSMPos = positionMap;
    m_RSMNrm = normalMap;
    m_RSMRad = radianceMap;
    if (!m_Device) return;
    if (m_RSMPos) m_Device->UpdateDescriptorSet(m_Set, kBindRSMPos,   rhi::DescriptorType::CombinedImageSampler, m_RSMPos, ResS(m_LinearSampler));
    if (m_RSMNrm) m_Device->UpdateDescriptorSet(m_Set, kBindRSMNrm,   rhi::DescriptorType::CombinedImageSampler, m_RSMNrm, ResS(m_LinearSampler));
    if (m_RSMRad) m_Device->UpdateDescriptorSet(m_Set, kBindRadiance, rhi::DescriptorType::CombinedImageSampler, m_RSMRad, ResS(m_LinearSampler));

    struct alignas(16) {
        float4x4 lightViewProj;
        float4   params;   // x=lightCount, y=光源类型, z=阴影强度, w=VPL 采样缩放
    } ub;
    ub.lightViewProj = lightViewProj;
    ub.params = float4(float(lightCount), shadowType, shadowStrength, vplScale);
    void* mapped = m_ParamsUBO->Map();
    if (mapped) {
        memcpy(mapped, &ub, sizeof(ub));
        m_ParamsUBO->Unmap();
    }
}

void RSMIndirect::PreBind(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_PSO.IsValid() || !m_Output.IsValid()) return;
    // 见头文件：绑管线必须早于 BeginOffscreenPass
    cmd->SetPipeline(ResP(m_PSO));
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Set);
    // 视口用输出纹理实际尺寸（半分辨率），与渲染目标一致
    const u32 ow = ResT(m_Output)->GetWidth();
    const u32 oh = ResT(m_Output)->GetHeight();
    cmd->SetFullscreenViewport(static_cast<u32>(ow), static_cast<u32>(oh));
}

void RSMIndirect::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_PSO.IsValid() || !m_Output.IsValid()) return;
    if (!m_Depth || !m_WorldPos || !m_Normal || !m_RSMPos || !m_RSMNrm || !m_RSMRad) return;
    cmd->Draw(3);   // 全屏三角（顶点着色器覆盖整个视口 ⇒ 每个像素都被写）
}

} // namespace he::render
