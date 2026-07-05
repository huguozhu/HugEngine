#include "Pipeline/GPUCulling.h"
#include "GPUCull.comp.spv.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include <cstring>
#include <glm/gtc/matrix_access.hpp>

// ============================================================
// GPUCulling 实现
// ============================================================

namespace he::render {

/// 从 ViewProj 矩阵提取 6 个视锥平面（Gribb-Hartmann, world space）
static void ExtractFrustumPlanes(const float4x4& vp, float4 planes[6]) {
    // 列主序矩阵，row = column in glm
    float4 r1 = glm::row(vp, 0);  // X
    float4 r2 = glm::row(vp, 1);  // Y
    float4 r3 = glm::row(vp, 2);  // Z
    float4 r4 = glm::row(vp, 3);  // W

    planes[0] = r4 + r1;  // Left
    planes[1] = r4 - r1;  // Right
    planes[2] = r4 + r2;  // Bottom
    planes[3] = r4 - r2;  // Top
    planes[4] = r4 + r3;  // Near (Vulkan z∈[0,1])
    planes[5] = r4 - r3;  // Far

    for (int i = 0; i < 6; ++i) {
        float len = glm::length(float3(planes[i]));
        planes[i] /= len;
    }
}

bool GPUCulling::Initialize(rhi::IRHIDevice* device) {
    m_CS.stage      = rhi::ShaderStage::Compute;
    m_CS.spirv      = k_GPUCull_comp_spv;
    m_CS.entryPoint = "main";

    // 描述符集布局: 3 个 SSBO binding
    rhi::DescriptorSetLayoutDesc layoutDesc;
    layoutDesc.bindings = {
        {0, rhi::DescriptorType::StorageBuffer, 1, 32},  // u_ObjectBounds (Compute)
        {1, rhi::DescriptorType::StorageBuffer, 1, 32},  // u_VisibleIndices
        {2, rhi::DescriptorType::StorageBuffer, 1, 32},  // u_DrawCount
    };
    m_DescLayout = device->CreateDescriptorSetLayout(layoutDesc);
    m_DescSet    = device->AllocateDescriptorSet(m_DescLayout);

    // SSBO 创建（binding 0 由 SetSceneBuffer 设置，此处创建占位）
    // VisibleIndices + DrawCount
    {
        rhi::BufferDesc d; d.size = sizeof(u32) * kMaxObjects;
        d.usage = rhi::BufferUsage::Storage; d.cpuAccess = true;
        m_VisibleIndicesBuf = device->CreateBuffer(d);
        device->UpdateDescriptorSet(m_DescSet, 1, rhi::DescriptorType::StorageBuffer, m_VisibleIndicesBuf.get());
    }
    {
        rhi::BufferDesc d; d.size = sizeof(u32) * 4;  // 16 bytes, 4 u32s for safety
        d.usage = rhi::BufferUsage::Storage; d.cpuAccess = true;
        m_DrawCountBuf = device->CreateBuffer(d);
        device->UpdateDescriptorSet(m_DescSet, 2, rhi::DescriptorType::StorageBuffer, m_DrawCountBuf.get());
    }

    // Compute PSO（Hi-Z: 6 planes + float4x4 VP + float2 screen + 2 uint）
    rhi::PushConstantRange pcRange;
    pcRange.stageMask = 32;
    pcRange.offset = 0;
    pcRange.size   = sizeof(float4) * 6 + sizeof(float4x4) + sizeof(float2) + sizeof(u32) * 4;

    rhi::PipelineStateDesc psoDesc;
    psoDesc.bindPoint = rhi::PipelineBindPoint::Compute;
    psoDesc.computeShader = &m_CS;
    psoDesc.pushConstantRanges = {pcRange};
    psoDesc.descriptorSetLayouts = {m_DescLayout};
    psoDesc.debugName = "GPUCull";

    m_PSO = device->CreatePipelineState(psoDesc);
    HE_ASSERT(m_PSO, "GPUCulling: Compute PSO creation failed");

    m_Initialized = true;
    HE_CORE_INFO("GPUCulling initialized (max {} objects)", kMaxObjects);
    return true;
}

void GPUCulling::Shutdown(rhi::IRHIDevice* device) {
    if (!m_Initialized) return;
    m_VisibleIndicesBuf.reset();
    m_DrawCountBuf.reset();
    m_PSO.reset();
    if (m_DescLayout != rhi::kInvalidLayout && device) {
        device->DestroyDescriptorSetLayout(m_DescLayout);
        m_DescLayout = rhi::kInvalidLayout;
    }
    m_DescSet = rhi::kInvalidSet;
    m_Initialized = false;
}

void GPUCulling::SetSceneBuffer(rhi::IRHIDevice* device, rhi::IRHIBuffer* gpuSceneSSBO) {
    if (!m_Initialized || !gpuSceneSSBO) return;
    // 绑定 GPUScene SSBO 到 binding 0
    device->UpdateDescriptorSet(m_DescSet, 0, rhi::DescriptorType::StorageBuffer, gpuSceneSSBO);
}

void GPUCulling::Dispatch(rhi::IRHICommandList* cmd,
                           const float4x4& viewProj, u32 objectCount,
                           u32 screenW, u32 screenH) {
    if (!m_Initialized || !enabled || objectCount == 0) return;

    float4 planes[6];
    ExtractFrustumPlanes(viewProj, planes);

    // Push constants: 匹配 GPUCull.comp.slang 布局
    struct { float4 planes[6]; float4x4 vp; float2 sSize; u32 mips; u32 count; } pc;
    for (int i=0;i<6;++i) pc.planes[i]=planes[i];
    pc.vp    = viewProj;
    pc.sSize = float2((float)screenW, (float)screenH);
    pc.mips  = 0;  // Hi-Z 暂未启用
    pc.count = objectCount;

    cmd->SetPipeline(m_PSO.get());
    cmd->BindDescriptorSet(0, m_DescSet);
    cmd->SetPushConstants(0, sizeof(pc), &pc);

    u32 groups = (objectCount + 63) / 64;
    cmd->Dispatch(groups, 1, 1);

    m_LastVisibleCount = 0;
}

void GPUCulling::Readback(rhi::IRHIDevice* device,
                           std::vector<u32>& outVisibleIndices) {
    outVisibleIndices.clear();
    if (!m_Initialized || !enabled) return;

    // 读回 count
    u32 count = 0;
    void* dc = m_DrawCountBuf->Map();
    if (dc) { count = *static_cast<u32*>(dc); m_DrawCountBuf->Unmap(); }

    if (count == 0) { m_LastVisibleCount = 0; return; }
    if (count > kMaxObjects) count = kMaxObjects;

    // 读回可见索引列表
    outVisibleIndices.resize(count);
    void* vi = m_VisibleIndicesBuf->Map();
    if (vi) {
        memcpy(outVisibleIndices.data(), vi, count * sizeof(u32));
        m_VisibleIndicesBuf->Unmap();
    }

    m_LastVisibleCount = count;
}

} // namespace he::render
