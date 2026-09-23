// PostProcess/SSAO.cpp — SSAO 实现
#include "PostProcess/SSAO.h"
#include "Pipeline/Camera.h"   // CameraData：SetCamera 注入的真实相机（GetProjMatrix）
#include "Core/Log.h"
#include "Core/Assert.h"
#include "SSAO.vert.spv.h"
#include "SSAO.frag.spv.h"
#include "GTAO.frag.spv.h"   // GTAO 模式（M6.3）
#include "SSAO_Blur.vert.spv.h"
#include "SSAO_Blur.frag.spv.h"
#include <glm/gtc/matrix_transform.hpp>
#include <random>
#include <cstring>

namespace he::render {

// SSAO 描述符集绑定号（与 SSAO.frag 一致）
static constexpr u32 kSSAOBindDepth  = 0;   // 深度
static constexpr u32 kSSAOBindNormal = 1;   // 法线
static constexpr u32 kSSAOBindNoise  = 2;   // 噪声纹理
static constexpr u32 kSSAOBindParams = 3;   // 参数 UBO

// SSAO 模糊集绑定号（与 SSAO_Blur.frag 一致）
static constexpr u32 kSSAOBlurBindInput = 0;   // 待模糊的 AO 纹理

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
    rhi::TextureDesc td;
    td.format=rhi::Format::RGBA16_FLOAT;
    td.width=size;
    td.height=size;
    td.mipLevels=1;
    td.usage=rhi::TextureUsage::ShaderResource;
    td.initialData=noise.data();
    m_NoiseTex = m_Device->CreateTexture(td);
}

void SSAO::CreateAOTexture(u32 w, u32 h) {
    rhi::TextureDesc td;
    td.format=rhi::Format::R16_FLOAT;
    td.width=w;
    td.height=h;
    td.mipLevels=1;
    td.usage=rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_AOTexture = m_Device->CreateTexture(td);
    rhi::SamplerDesc sd;
    sd.minFilter=sd.magFilter=rhi::FilterMode::Linear;
    sd.addressU=sd.addressV=rhi::AddressMode::ClampToEdge;
    m_AOSampler = m_Device->CreateSampler(sd);
}

/// 创建「原始 AO」纹理：SSAO/GTAO 直接写入这里，随后由 Blur pass 采样。
/// 之所以与最终 AO 分开两张：Blur 不能采样自己正在写的附件（attachment feedback loop）。
void SSAO::CreateRawAOTexture(u32 w, u32 h) {
    rhi::TextureDesc td;
    td.format=rhi::Format::R16_FLOAT;
    td.width=w;
    td.height=h;
    td.mipLevels=1;
    td.usage=rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
    m_RawAOTexture = m_Device->CreateTexture(td);

    // Blur pass 的输入就是这张纹理：地址只在重建时变，故在这里写一次描述符。
    // 【为什么不再每帧写】描述符集是 GPU 执行期读取的对象，在 kMaxFramesInFlight=3 下
    //   每帧改写同一份会和正在执行的帧争用（与参数 UBO 同类风险），而输入地址其实没变。
    if (m_BlurSet != rhi::kInvalidSet && m_AOSampler) {
        m_Device->UpdateDescriptorSet(m_BlurSet, kSSAOBlurBindInput,
            rhi::DescriptorType::CombinedImageSampler, m_RawAOTexture.get(), m_AOSampler.get());
    }
}

bool SSAO::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width = width;
    m_Height = height;

    GenerateKernel();
    GenerateNoise(4);

    // SSAO PSO — 描述符布局 + 参数 UBO 在 Init 中创建，PSO 本体惰性编译
    {
        rhi::DescriptorSetLayoutDesc l;
        l.bindings = {{kSSAOBindDepth,rhi::DescriptorType::CombinedImageSampler,1,16},  // Depth
                      {kSSAOBindNormal,rhi::DescriptorType::CombinedImageSampler,1,16},  // Normal
                      {kSSAOBindNoise,rhi::DescriptorType::CombinedImageSampler,1,16},  // Noise
                      {kSSAOBindParams,rhi::DescriptorType::UniformBuffer,1,16}};         // Params
        m_SSAOLayout = device->CreateDescriptorSetLayout(l);

        // 【按帧在飞分槽（2026-09 画质阶段 0 修复）】每槽一份描述符集 + 一份参数 UBO：
        //   写第 N+1 帧时不会覆盖第 N 帧仍在读的那一份（修复前是单份 + 每帧重写 ⇒ AO 逐帧不确定，
        //   详见 `SSAO.h` 描述符集处的说明）。范式与 `LightingPass::m_BlendUBO/m_Sets` 一致。
        rhi::BufferDesc ubDesc;
        ubDesc.size  = 64 * sizeof(float4) + sizeof(float4) + sizeof(float4x4) * 2;
        ubDesc.usage = rhi::BufferUsage::Uniform;
        ubDesc.cpuAccess = true;
        for (u32 i = 0; i < rhi::kMaxFramesInFlight; ++i) {
            m_SSAOSets[i] = device->AllocateDescriptorSet(m_SSAOLayout);
            m_ParamUBO[i] = device->CreateBuffer(ubDesc);
            device->UpdateDescriptorSet(m_SSAOSets[i], kSSAOBindParams,
                                        rhi::DescriptorType::UniformBuffer, m_ParamUBO[i].get());
        }

        // 存储 ShaderBytecode 副本（供惰性创建 PSO 时使用）
        m_SSAO_VS.stage = rhi::ShaderStage::Vertex;
        m_SSAO_VS.spirv = k_SSAO_vert_spv;
        m_SSAO_VS.entryPoint = "vertexMain";
        m_SSAO_FS.stage = rhi::ShaderStage::Pixel;
        m_SSAO_FS.spirv = k_SSAO_frag_spv;
        m_SSAO_FS.entryPoint = "fragmentMain";

        rhi::PipelineStateDesc d;
        d.vertexShader = &m_SSAO_VS;
        d.pixelShader = &m_SSAO_FS;
        d.topology = rhi::PrimitiveTopology::TriangleList;
        d.depthTest = false;
        d.depthWrite = false;
        d.depthFormat = rhi::Format::Unknown;
        d.colorAttachmentCount = 1;
        d.colorFormats[0] = rhi::Format::R16_FLOAT;
        d.descriptorSetLayouts = {m_SSAOLayout}; d.debugName = "SSAO";
        m_SSAO_PsoDesc = d;  // 保存描述符供惰性创建

        // 注册到 PSO 预热队列（后台线程将预编译到 VkPipelineCache）
        device->PrecompileQueuePSO(d);
    }

    // GTAO PSO（M6.3）——复用同一描述符布局与参数 UBO，仅替换片段着色器
    // （顶点着色器沿用 SSAO 的全屏三角，UBO 布局一致：u_Params.w 复用为切片数）
    {
        m_GTAO_FS.stage = rhi::ShaderStage::Pixel;
        m_GTAO_FS.spirv = k_GTAO_frag_spv;
        m_GTAO_FS.entryPoint = "fragmentMain";

        rhi::PipelineStateDesc d;
        d.vertexShader = &m_SSAO_VS;
        d.pixelShader = &m_GTAO_FS;
        d.topology = rhi::PrimitiveTopology::TriangleList;
        d.depthTest = false;
        d.depthWrite = false;
        d.depthFormat = rhi::Format::Unknown;
        d.colorAttachmentCount = 1;
        d.colorFormats[0] = rhi::Format::R16_FLOAT;
        d.descriptorSetLayouts = {m_SSAOLayout};
        d.debugName = "GTAO";
        m_GTAO_PsoDesc = d;

        device->PrecompileQueuePSO(d);
    }

    // Blur PSO — 同样采用惰性创建模式
    {
        rhi::DescriptorSetLayoutDesc l;
        l.bindings = {{kSSAOBlurBindInput,rhi::DescriptorType::CombinedImageSampler,1,16}};
        m_BlurLayout = device->CreateDescriptorSetLayout(l);
        m_BlurSet    = device->AllocateDescriptorSet(m_BlurLayout);

        m_Blur_VS.stage = rhi::ShaderStage::Vertex;
        m_Blur_VS.spirv = k_SSAO_Blur_vert_spv;
        m_Blur_VS.entryPoint = "vertexMain";
        m_Blur_FS.stage = rhi::ShaderStage::Pixel;
        m_Blur_FS.spirv = k_SSAO_Blur_frag_spv;
        m_Blur_FS.entryPoint = "fragmentMain";

        rhi::PipelineStateDesc d;
        d.vertexShader = &m_Blur_VS;
        d.pixelShader = &m_Blur_FS;
        d.topology = rhi::PrimitiveTopology::TriangleList;
        d.depthTest = false;
        d.depthWrite = false;
        d.depthFormat = rhi::Format::Unknown;
        d.colorAttachmentCount = 1;
        d.colorFormats[0] = rhi::Format::R16_FLOAT;
        d.descriptorSetLayouts = {m_BlurLayout}; d.debugName = "SSAO_Blur";
        m_Blur_PsoDesc = d;

        device->PrecompileQueuePSO(d);
    }

    // 输出纹理 + 采样器
    rhi::SamplerDesc ptSamp;
    ptSamp.minFilter=ptSamp.magFilter=rhi::FilterMode::Nearest;
    ptSamp.addressU=ptSamp.addressV=rhi::AddressMode::ClampToEdge;
    m_PointSampler = device->CreateSampler(ptSamp);

    CreateAOTexture(halfResW(width), halfResH(height));
    CreateRawAOTexture(halfResW(width), halfResH(height));

    m_Ready = true;
    HE_CORE_INFO("SSAO initialized ({}×{})", width, height);
    return true;
}

void SSAO::Shutdown() {
    if (m_Device && m_SSAOLayout!=rhi::kInvalidLayout) m_Device->DestroyDescriptorSetLayout(m_SSAOLayout);
    if (m_Device && m_BlurLayout!=rhi::kInvalidLayout) m_Device->DestroyDescriptorSetLayout(m_BlurLayout);
    m_SSAO_PSO.reset();
    m_Blur_PSO.reset();
    m_AOTexture.reset();
    m_RawAOTexture.reset();
    m_AOSampler.reset();
    m_PointSampler.reset();
    m_NoiseTex.reset();
    for (u32 i = 0; i < rhi::kMaxFramesInFlight; ++i) m_ParamUBO[i].reset();
    m_InputsBound = false;
    m_Device = nullptr;
    m_Ready = false;
}

void SSAO::OnResize(u32 w, u32 h) { m_Width=w; m_Height=h; CreateAOTexture(halfResW(w), halfResH(h)); CreateRawAOTexture(halfResW(w), halfResH(h)); }

void SSAO::SetInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal) {
    // 【只在输入真的变了才写描述符】GBuffer 的深度/法线跨帧稳定 ⇒ 缓存比较可把"每帧写描述符"
    //   降为"输入变化时写一次"，顺带消除描述符集被正在执行的帧读到中间状态的风险。
    //   写入时覆盖**全部帧槽**的集合（输入对所有槽位都相同）。
    const bool changed = (depth != m_DepthTex) || (normal != m_NormalTex) || !m_InputsBound;
    m_DepthTex  = depth;
    m_NormalTex = normal;
    if (!changed) return;
    for (u32 i = 0; i < rhi::kMaxFramesInFlight; ++i) {
        if (m_SSAOSets[i] == rhi::kInvalidSet) continue;
        if (m_DepthTex)  m_Device->UpdateDescriptorSet(m_SSAOSets[i], kSSAOBindDepth,  rhi::DescriptorType::CombinedImageSampler, m_DepthTex,  m_PointSampler.get());
        if (m_NormalTex) m_Device->UpdateDescriptorSet(m_SSAOSets[i], kSSAOBindNormal, rhi::DescriptorType::CombinedImageSampler, m_NormalTex, m_PointSampler.get());
        m_Device->UpdateDescriptorSet(m_SSAOSets[i], kSSAOBindNoise, rhi::DescriptorType::CombinedImageSampler, m_NoiseTex.get(), m_PointSampler.get());
    }
    m_InputsBound = true;
}

void SSAO::PreBind(rhi::IRHICommandList* cmd) {
    if (!m_Ready) return;
    // 惰性创建 PSO（首次调用时，VkPipelineCache 可能已被后台预热）
    if (useGTAO) {
        if (!m_GTAO_PSO) m_GTAO_PSO = m_Device->CreatePipelineState(m_GTAO_PsoDesc);
        cmd->SetPipeline(m_GTAO_PSO.get());
    } else {
        if (!m_SSAO_PSO) m_SSAO_PSO = m_Device->CreatePipelineState(m_SSAO_PsoDesc);
        cmd->SetPipeline(m_SSAO_PSO.get());
    }
}

void SSAO::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_DepthTex || !m_NormalTex || !enabled) return;

    // 惰性创建 PSO（首次渲染时，VkPipelineCache 已被后台预热，创建耗时 ~2ms 而非 ~50ms）
    if (useGTAO) {
        if (!m_GTAO_PSO) m_GTAO_PSO = m_Device->CreatePipelineState(m_GTAO_PsoDesc);
    } else if (!m_SSAO_PSO) {
        m_SSAO_PSO = m_Device->CreatePipelineState(m_SSAO_PsoDesc);
    }
    if (!m_Blur_PSO) {
        m_Blur_PSO = m_Device->CreatePipelineState(m_Blur_PsoDesc);
    }

    // 视口/附件尺寸一律用 AO 纹理实际尺寸（halfRes 时为半分辨率）
    const u32 aoW = m_AOTexture->GetWidth();
    const u32 aoH = m_AOTexture->GetHeight();

    // 【为什么拆成两个 render pass（2026-09 画质阶段 0 修复）】
    //   此前 SSAO 与 Blur 共处**同一个** render pass：Blur 一边把 `m_AOTexture` 当颜色附件写，
    //   一边通过描述符采样**同一张**纹理（attachment feedback loop）。Vulkan 明确规定在同一
    //   subpass 内采样自己正在写的附件是**未定义行为**，结果取决于驱动的 tile/缓存行为
    //   ⇒ **逐帧不确定**。实测同一配置两次运行 `hdr` 差 5.8 万像素、
    //   `prov0_ao_*` 差 2.2 万像素；把 AO 的层栈应用权重置 0（该 pass 不注册）后全部转储逐位相同
    //   ⇒ 抖动确实源自本 pass，不是"设计上的随机"。
    //   现改为标准两趟：Pass 1 写 `m_RawAOTexture` → 显式屏障 RT→SRV → Pass 2 读原始 AO、写最终 AO。
    //   （原先已存在的 `m_BlurTexture` 成员正是为此准备的，但一直没被用上。）
    //   对外的最终输出仍是 `m_AOTexture`，消费者（Lighting/帧图）无需改动。
    // 【为什么 pass 在这里开关而不是帧图 lambda 里】两趟各有自己的附件与 PSO，且中间必须插屏障，
    //   帧图只声明"本 pass 写最终 AO"这一个资源，内部趟次由本函数自管（同 `AA_SMAA::Render`）。
    rhi::ClearValue aoClear;
    aoClear.color[0]=aoClear.color[1]=aoClear.color[2]=aoClear.color[3]=1.0f;

    // --- Pass 1: SSAO / GTAO → m_RawAOTexture（按模式选择 PSO）---
    cmd->SetPipeline(useGTAO ? m_GTAO_PSO.get() : m_SSAO_PSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_SSAOSets[m_FrameSlot]);
    cmd->SetViewport({0,(float)aoH,(float)aoW,-(float)aoH,0,1});
    cmd->SetScissor({0,0,aoW,aoH});

    // 上传 SSAO 参数到 Uniform Buffer（kernel[64] + params + proj）
    // 对齐 shader 中 SSAOParams cbuffer 布局
    {
        u8* dst = static_cast<u8*>(m_ParamUBO[m_FrameSlot]->Map());
        if (dst) {
            // kernel[64] — 半球采样方向（view-space）
            memcpy(dst, m_Kernel.data(), 64 * sizeof(float4));
            dst += 64 * sizeof(float4);
            // params — SSAO: x=radius,y=bias,z=intensity,w=sampleCount
            //          GTAO: x=radius,y=bias,z=intensity,w=sliceCount（复用同一 UBO 布局）
            float4 p(radius, bias, intensity,
                     useGTAO ? float(sliceCount) : float(sampleCount));
            memcpy(dst, &p, sizeof(float4));
            dst += sizeof(float4);
            // u_InvProj: 逆投影矩阵（clip→view，用于从深度重建 view-space 位置）
            // u_Proj:    正投影矩阵（view→clip，用于将采样点投影到屏幕）
            // 两者都取自**真实相机**：深度图是用它的投影渲染的，重建必须同源，
            // 否则非默认相机下 AO 的采样位置会系统性错位（§9.2-E）。
            float4x4 proj;
            if (m_Camera) {
                proj = m_Camera->GetProjMatrix();
            } else {
                float a = float(m_Width) / float(m_Height);
                proj = glm::perspectiveRH_ZO(glm::radians(kDefaultFOV), a,
                                             kDefaultNearPlane, kDefaultFarPlane);
            }
            float4x4 projInv = glm::inverse(proj);
            memcpy(dst, &projInv, sizeof(float4x4));
            dst += sizeof(float4x4);
            memcpy(dst, &proj, sizeof(float4x4));
            m_ParamUBO[m_FrameSlot]->Unmap();
        }
    }

    cmd->BeginOffscreenPass(m_RawAOTexture->GetNativeHandle(), nullptr, aoW, aoH, &aoClear, false);
    cmd->Draw(3);
    cmd->EndOffscreenPass();

    // 原始 AO 布局转换：RenderTarget → ShaderResource（Pass 2 要采样它）。
    // render pass 自身的 finalLayout 转变不经过 barrier，必须显式发一条（同 `AA_SMAA::Render`）。
    cmd->PipelineBarrier(rhi::PipelineStage::ColorAttachmentOutput,
                         rhi::PipelineStage::FragmentShader,
                         rhi::ResourceState::RenderTarget,
                         rhi::ResourceState::ShaderResource,
                         m_RawAOTexture.get());

    // --- Pass 2: Blur（读 m_RawAOTexture → 写 m_AOTexture）---
    cmd->SetPipeline(m_Blur_PSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_BlurSet);
    cmd->SetViewport({0,(float)aoH,(float)aoW,-(float)aoH,0,1});
    cmd->SetScissor({0,0,aoW,aoH});

    struct { float2 ts; float _pad[2]; } bpc;
    bpc.ts = float2(1.0f/float(aoW), 1.0f/float(aoH));   // 模糊半径按 AO 纹理实际尺寸
    cmd->SetPushConstants(0, sizeof(bpc), &bpc);

    cmd->BeginOffscreenPass(m_AOTexture->GetNativeHandle(), nullptr, aoW, aoH, &aoClear, false);
    cmd->Draw(3);
    cmd->EndOffscreenPass();
}

} // namespace he::render
