// GI/GI_DDGI.cpp — 动态探针 GI（Compute Shader 更新 + 时间混合 + 探针网格）
#include "GI/GI_DDGI.h"
#include "Core/Log.h"
#include "Subsystem/RenderSubsystem.h"
#include "DDGI.comp.spv.h"
#include <cstring>

namespace he::render {

// 计算调度所需的 Dispatch 组数（每线程处理一个探针，64 线程/组）
static u32 DispatchGroupCount(u32 probeCount) {
    return (probeCount + 63) / 64;
}

bool GI_DDGI::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    m_Device = device;
    m_Width = width; m_Height = height;
    m_Settings.enabled   = false;
    m_Settings.intensity = 1.0f;
    m_Settings.mode      = GIMode::DDGI;

    u32 probeCount = gridX * gridY * gridZ;
    u64 bufferSize = probeCount * kFloats4PerProbe * sizeof(float4);  // 每探针 256 字节

    // ---- 探针数据存储（两帧：当前 + 历史，每帧交换） ----
    rhi::BufferDesc probeDesc;
    probeDesc.size  = bufferSize;
    probeDesc.usage = rhi::BufferUsage::Storage;
    m_ProbeBuffer  = device->CreateBuffer(probeDesc);
    m_ProbeHistory = device->CreateBuffer(probeDesc);

    // ---- 探针网格参数 Uniform Buffer（CPU 可写，每帧更新） ----
    rhi::BufferDesc uniformDesc;
    uniformDesc.size      = sizeof(ProbeGridUniform);
    uniformDesc.usage     = rhi::BufferUsage::Uniform;
    uniformDesc.cpuAccess = true;  // 每帧 Map/Unmap 更新
    m_GridUniform = device->CreateBuffer(uniformDesc);

    // ---- 采样器（点采样，用于 GBuffer 读取） ----
    rhi::SamplerDesc psd;
    psd.minFilter  = psd.magFilter = rhi::FilterMode::Nearest;
    psd.addressU   = psd.addressV   = rhi::AddressMode::ClampToEdge;
    m_PointSampler = device->CreateSampler(psd);

    // ---- DescriptorSet 布局 ----
    // binding 0-2: GBuffer CombinedImageSampler（Compute 阶段）
    // binding 3:   ProbeBuffer  StorageBuffer（当前帧输出，RW）
    // binding 4:   GridUniform  UniformBuffer（探针网格参数）
    // binding 5:   HistoryBuffer StorageBuffer（上一帧输入，只读）
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        {0, rhi::DescriptorType::CombinedImageSampler, 1, 32},   // GBufferA
        {1, rhi::DescriptorType::CombinedImageSampler, 1, 32},   // GBufferB
        {2, rhi::DescriptorType::CombinedImageSampler, 1, 32},   // Depth
        {3, rhi::DescriptorType::StorageBuffer,         1, 32},  // ProbeBuffer (RW)
        {4, rhi::DescriptorType::UniformBuffer,         1, 32},  // GridParams
        {5, rhi::DescriptorType::StorageBuffer,         1, 32},  // ProbeHistory (只读)
    };
    m_Layout = device->CreateDescriptorSetLayout(layout);
    m_Set    = device->AllocateDescriptorSet(m_Layout);

    // 预绑定不变的 binding：uniform buffer（每帧只需 Map/Unmap 更新内容）
    device->UpdateDescriptorSet(m_Set, 4, rhi::DescriptorType::UniformBuffer, m_GridUniform.get());

    // ---- Compute PSO ----
    rhi::ShaderBytecode cs;
    cs.stage      = rhi::ShaderStage::Compute;
    cs.spirv      = k_DDGI_comp_spv;
    cs.entryPoint = "main";

    rhi::PipelineStateDesc psoDesc;
    psoDesc.bindPoint            = rhi::PipelineBindPoint::Compute;
    psoDesc.computeShader        = &cs;
    psoDesc.descriptorSetLayouts = {m_Layout};
    psoDesc.debugName            = "DDGI";
    m_PSO = device->CreatePipelineState(psoDesc);

    if (!m_PSO) {
        HE_CORE_ERROR("GI_DDGI: Compute PSO creation failed");
        return false;
    }

    m_Ready = true;
    HE_CORE_INFO("GI_DDGI initialized ({}×{}×{} = {} probes, {} KB, temporal={})",
        gridX, gridY, gridZ, probeCount,
        (bufferSize * 2) / 1024, blendAlpha);
    return true;
}

void GI_DDGI::Shutdown() {
    if (m_Device && m_Layout != rhi::kInvalidLayout) {
        m_Device->DestroyDescriptorSetLayout(m_Layout);
    }
    m_PSO.reset();
    m_ProbeBuffer.reset();
    m_ProbeHistory.reset();
    m_GridUniform.reset();
    m_PointSampler.reset();
    m_Device = nullptr;
    m_Ready  = false;
    HE_CORE_INFO("GI_DDGI shutdown");
}

void GI_DDGI::OnResize(u32 w, u32 h) {
    m_Width = w; m_Height = h;
}

void GI_DDGI::SetGBufferInputs(rhi::IRHITexture* depth, rhi::IRHITexture* normal, rhi::IRHITexture* albedo) {
    m_Depth  = depth;
    m_Normal = normal;
    m_Albedo = albedo;

    // 更新描述符集中 GBuffer 绑定（每帧纹理可能变化）
    if (m_Albedo && m_Device) {
        m_Device->UpdateDescriptorSet(m_Set, 0, rhi::DescriptorType::CombinedImageSampler,
            m_Albedo, m_PointSampler.get());
    }
    if (m_Normal && m_Device) {
        m_Device->UpdateDescriptorSet(m_Set, 1, rhi::DescriptorType::CombinedImageSampler,
            m_Normal, m_PointSampler.get());
    }
    if (m_Depth && m_Device) {
        m_Device->UpdateDescriptorSet(m_Set, 2, rhi::DescriptorType::CombinedImageSampler,
            m_Depth, m_PointSampler.get());
    }
}

void GI_DDGI::Update(const SubsystemContext& ctx) {
    // 从 SubsystemContext 获取相机位置和投影矩阵（用于探针→屏幕投影）
    if (ctx.camera) {
        m_CameraPos   = ctx.camera->position;
        m_ViewProj    = ctx.camera->GetViewProjMatrix();
        m_CameraReady = true;
    }
}

void GI_DDGI::Render(rhi::IRHICommandList* cmd) {
    if (!m_Ready || !m_Settings.enabled) return;
    if (!m_Depth || !m_Normal || !m_Albedo) return;

    u32 probeCount = gridX * gridY * gridZ;

    // 球面采样数
    static const u32 kNumSamples = 32;
    // 时间混合历史有效性（首帧无历史）
    static bool s_FirstFrame = true;

    // 更新 descriptor set：绑定当前输出缓冲和历史缓冲（每帧因 swap 而变化）
    m_Device->UpdateDescriptorSet(m_Set, 3, rhi::DescriptorType::StorageBuffer, m_ProbeBuffer.get());
    m_Device->UpdateDescriptorSet(m_Set, 5, rhi::DescriptorType::StorageBuffer, m_ProbeHistory.get());

    // ---- 上传探针网格 Uniform（含时间混合参数） ----
    ProbeGridUniform uniforms;
    uniforms.gridOrigin = float4(gridOrigin, 0.0f);
    uniforms.gridSize   = float4(float(gridX), float(gridY), float(gridZ), cellSize);
    uniforms.cameraPos  = float4(m_CameraPos, 0.0f);
    uniforms.params     = float4(m_Settings.intensity,
                                 float(kNumSamples),
                                 blendAlpha,
                                 s_FirstFrame ? 0.0f : 1.0f);  // w=historyValid
    uniforms.viewProj   = m_ViewProj;

    void* mapped = m_GridUniform->Map();
    if (mapped) {
        memcpy(mapped, &uniforms, sizeof(ProbeGridUniform));
        m_GridUniform->Unmap();
    }

    // ---- Compute Dispatch ----
    cmd->SetPipeline(m_PSO.get());
    cmd->BindDescriptorSet(0, m_Set);
    cmd->Dispatch(DispatchGroupCount(probeCount), 1, 1);

    // 交换当前帧与历史帧缓冲（下一帧用本次结果作为历史）
    m_ProbeBuffer.swap(m_ProbeHistory);
    s_FirstFrame = false;

    // 全局 Barrier：确保 Compute Shader 所有 UAV 写入完成后，后续 Fragment Shader 可读取
    cmd->PipelineBarrier(
        rhi::PipelineStage::ComputeShader,
        rhi::PipelineStage::FragmentShader,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::ShaderResource);
}

} // namespace he::render
