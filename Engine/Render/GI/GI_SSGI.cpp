// PostProcess/SSGI.cpp — 屏幕空间全局光照
#include "GI/GI_SSGI.h"
#include "Core/Log.h"
#include "SSAO.vert.spv.h"
#include "SSGI.frag.spv.h"
#include <glm/gtc/matrix_transform.hpp>
#include <random>
#include <cstring>

namespace he::render {

// SSGI 描述符集绑定号（与 SSGI.frag 的 vk::binding 一致）
static constexpr u32 kSSGIBindDepth  = 0;   // 深度
static constexpr u32 kSSGIBindNormal = 1;   // 法线
static constexpr u32 kSSGIBindAlbedo = 2;   // 反照率
static constexpr u32 kSSGIBindParams = 3;   // 参数 Uniform Buffer

// SSGI 半球采样核大小（CPU 生成随机方向，GPU 逐采样点求间接光）
static constexpr u32 kSSGIKernelSize = 32;

// 生成 SSGI 半球采样核：均匀随机方向 + 径向缩放（近核密集、远核稀疏）
static void GenSSGISamples(std::vector<float4>& kernel, int count) {
    kernel.resize(kSSGIKernelSize);
    std::default_random_engine gen(42);   // 固定种子：采样核跨帧稳定，避免闪烁
    std::uniform_real_distribution<float> rnd(0, 1);
    for (int i = 0; i < count && i < static_cast<int>(kSSGIKernelSize); i++) {
        // 半球随机方向（z 偏正，向上半球）
        float3 s(rnd(gen) * 2 - 1, rnd(gen) * 2 - 1, rnd(gen));
        s = glm::normalize(s) * rnd(gen);
        // 采样距离按样本索引递增（近处密、远处疏，覆盖不同尺度的间接光）
        float scale = float(i) / float(count);
        scale = glm::mix(0.1f, 1.0f, scale * scale);
        kernel[i] = float4(s * scale, 0);
    }
}

bool GI_SSGI::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    // 1. 保存设备与分辨率，设置默认 GI 参数
    m_Device = device;
    m_Width  = width;
    m_Height = height;
    m_Settings.enabled   = false;
    m_Settings.intensity = 1.0f;
    m_Settings.mode      = GIMode::SSGI;

    // 2. 创建 Uniform Buffer（656 字节：kernel[32]×16 + params[16] + invProj[64] + proj[64]）
    rhi::BufferDesc ubDesc;
    ubDesc.size      = 32 * sizeof(float4) + sizeof(float4) + 2 * sizeof(float4x4);
    ubDesc.usage     = rhi::BufferUsage::Uniform;
    ubDesc.cpuAccess = true;
    m_UniformBuffer  = device->CreateBuffer(ubDesc);

    // 3. 创建描述符集（binding 0-2：深度/法线/反照率，binding 3：Uniform Buffer）
    rhi::DescriptorSetLayoutDesc layoutDesc;
    layoutDesc.bindings = {
        {kSSGIBindDepth,  rhi::DescriptorType::CombinedImageSampler, 1, 16},
        {kSSGIBindNormal, rhi::DescriptorType::CombinedImageSampler, 1, 16},
        {kSSGIBindAlbedo, rhi::DescriptorType::CombinedImageSampler, 1, 16},
        {kSSGIBindParams, rhi::DescriptorType::UniformBuffer, 1, 16},
    };
    m_DescLayout = device->CreateDescriptorSetLayout(layoutDesc);
    m_DescSet    = device->AllocateDescriptorSet(m_DescLayout);

    // Uniform Buffer 固定绑定到参数 binding（每帧只更新内容，不重绑）
    device->UpdateDescriptorSet(m_DescSet, kSSGIBindParams, rhi::DescriptorType::UniformBuffer, m_UniformBuffer.get());

    // 4. 编译管线（顶点 = 全屏三角，像素 = SSGI 计算）
    rhi::ShaderBytecode vs, fs;
    vs.stage      = rhi::ShaderStage::Vertex;
    vs.spirv      = k_SSAO_vert_spv;
    vs.entryPoint = "vertexMain";
    fs.stage      = rhi::ShaderStage::Pixel;
    fs.spirv      = k_SSGI_frag_spv;
    fs.entryPoint = "fragmentMain";

    rhi::PipelineStateDesc psoDesc;
    psoDesc.vertexShader        = &vs;
    psoDesc.pixelShader         = &fs;
    psoDesc.topology            = rhi::PrimitiveTopology::TriangleList;
    psoDesc.depthTest           = false;   // 后处理全屏 pass，不读写深度
    psoDesc.depthWrite          = false;
    psoDesc.depthFormat         = rhi::Format::Unknown;
    psoDesc.colorAttachmentCount = 1;
    psoDesc.colorFormats[0]     = rhi::Format::RGBA16_FLOAT;
    psoDesc.descriptorSetLayouts = {m_DescLayout};
    psoDesc.debugName           = "SSGI";
    m_PSO = device->CreatePipelineState(psoDesc);

    // 5. 创建点采样器（GBuffer 精确读取）
    rhi::SamplerDesc samplerDesc;
    samplerDesc.minFilter = samplerDesc.magFilter = rhi::FilterMode::Nearest;
    samplerDesc.addressU  = samplerDesc.addressV  = rhi::AddressMode::ClampToEdge;
    m_PointSampler = device->CreateSampler(samplerDesc);

    // 6. 创建输出纹理（halfRes 时降半，省约 3/4 像素着色）
    CreateOutputTex(halfResW(width), halfResH(height));
    m_Ready = true;
    HE_CORE_INFO("SSGI initialized ({}x{})", m_Output->GetWidth(), m_Output->GetHeight());
    return true;
}

void GI_SSGI::Shutdown() {
    m_PSO.reset();
    m_UniformBuffer.reset();
    if (m_Device && m_DescLayout != rhi::kInvalidLayout) {
        m_Device->DestroyDescriptorSetLayout(m_DescLayout);
    }
    m_Output.reset();
    m_Sampler.reset();
    m_PointSampler.reset();
    m_Device = nullptr;
    m_Ready  = false;
}

void GI_SSGI::OnResize(u32 w, u32 h) {
    m_Width  = w;
    m_Height = h;
    CreateOutputTex(halfResW(w), halfResH(h));
}

void GI_SSGI::CreateOutputTex(u32 w, u32 h) {
    rhi::TextureDesc td;
    td.format    = rhi::Format::RGBA16_FLOAT;
    td.width     = w;
    td.height    = h;
    td.mipLevels = 1;
    td.usage     = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_Output = m_Device->CreateTexture(td);

    // 线性采样器：Lighting 阶段对 SSGI 结果做双线性上采样
    rhi::SamplerDesc samplerDesc;
    samplerDesc.minFilter = samplerDesc.magFilter = rhi::FilterMode::Linear;
    samplerDesc.addressU  = samplerDesc.addressV  = rhi::AddressMode::ClampToEdge;
    m_Sampler = m_Device->CreateSampler(samplerDesc);
}

void GI_SSGI::SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal, rhi::IRHITexture* albedo) {
    m_Depth  = depth;
    m_Normal = normal;
    m_Albedo = albedo;
    // 每帧纹理可能变化，动态更新描述符集绑定
    if (m_Depth) {
        m_Device->UpdateDescriptorSet(m_DescSet, kSSGIBindDepth, rhi::DescriptorType::CombinedImageSampler,
            m_Depth, m_PointSampler.get());
    }
    if (m_Normal) {
        m_Device->UpdateDescriptorSet(m_DescSet, kSSGIBindNormal, rhi::DescriptorType::CombinedImageSampler,
            m_Normal, m_PointSampler.get());
    }
    if (m_Albedo) {
        m_Device->UpdateDescriptorSet(m_DescSet, kSSGIBindAlbedo, rhi::DescriptorType::CombinedImageSampler,
            m_Albedo, m_PointSampler.get());
    }
}

void GI_SSGI::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_Settings.enabled || !m_Depth || !m_Normal || !m_Albedo) {
        return;
    }

    // 1. 绑定管线 + 描述符集，设置视口/裁剪
    cmd->SetPipeline(m_PSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_DescSet);
    // 视口用输出纹理实际尺寸（halfRes 时为半分辨率，与渲染目标一致）
    u32 ow = m_Output->GetWidth();
    u32 oh = m_Output->GetHeight();
    cmd->SetViewport({0, (float)oh, (float)ow, -(float)oh, 0, 1});
    cmd->SetScissor({0, 0, ow, oh});

    // 2. 生成采样核（首次生成，之后复用；通过 UBO 传递避免 push constant 溢出 256 字节限制）
    static std::vector<float4> kernel;
    if (kernel.empty()) {
        GenSSGISamples(kernel, kSSGIKernelSize);
    }

    // 3. 填充 Uniform Buffer（采样核 + 参数 + 投影矩阵）
    struct alignas(16) {
        float4   k[32];        // 采样核（32 个方向）
        float4   p;            // x=半径, y=强度, z=采样数
        float4x4 invProj;      // 逆投影：clip→view，重建 view-space
        float4x4 proj;         // 正投影：view→clip，采样点投影到屏幕
    } ub;
    memcpy(ub.k, kernel.data(), kSSGIKernelSize * sizeof(float4));
    ub.p = float4(radius, m_Settings.intensity, float(sampleCount), 0);
    float aspect = float(m_Width) / float(m_Height);
    float4x4 proj = glm::perspectiveRH_ZO(glm::radians(kDefaultFOV), aspect, kDefaultNearPlane, kDefaultFarPlane);
    ub.invProj = glm::inverse(proj);
    ub.proj    = proj;

    void* mapped = m_UniformBuffer->Map();
    if (mapped) {
        memcpy(mapped, &ub, sizeof(ub));
        m_UniformBuffer->Unmap();
    }

    // 4. 绘制全屏三角（3 顶点覆盖全屏）
    cmd->Draw(3);
}

} // namespace he::render
