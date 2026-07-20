#include "Pipeline/GPUCulling.h"
#include "Pipeline/MeshBatcher.h"  // IndirectDrawCommand
#include "GPUCull.comp.spv.h"
#include "GPUCull_Phase1.comp.spv.h"
#include "GPUCull_TwoPhase.comp.spv.h"
#include "HiZDownsample.comp.spv.h"
#include "PersistentCull.comp.spv.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include <cstring>
#include <glm/gtc/matrix_access.hpp>

// ============================================================
// GPUCulling 实现 — 单阶段/两阶段遮挡剔除
// ============================================================

namespace he::render {

// PTGParams — 持久化线程组每帧参数结构体
// 布局必须与 PersistentCull.comp.slang 中的 PTGParams 一致
struct PTGParams {
    float4x4 viewProj;          // [0..64)   投影矩阵
    float4   frustumPlanes[6];  // [64..160) 视锥平面
    float2   screenSize;        // [160..168)
    u32      objectCount;       // [168]
    u32      hizMipCount;       // [172]
    u32      frameIndex;        // [176]
    u32      _padPTG[3];        // [180..192) 填充到 192 字节（16 字节对齐）
};
static_assert(sizeof(PTGParams) == 192,
              "PTGParams size must be 192 bytes (padded to 16)");

/// 从 ViewProj 矩阵提取 6 个视锥平面（Gribb-Hartmann, world space）
static void ExtractFrustumPlanes(const float4x4& vp, float4 planes[6]) {
    float4 r1 = glm::row(vp, 0);
    float4 r2 = glm::row(vp, 1);
    float4 r3 = glm::row(vp, 2);
    float4 r4 = glm::row(vp, 3);

    planes[0] = r4 + r1;  // Left
    planes[1] = r4 - r1;  // Right
    planes[2] = r4 + r2;  // Bottom
    planes[3] = r4 - r2;  // Top
    // Vulkan [0,1]: 近平面 z>=0 → row2，非 OpenGL row3+row2 (z>=-w)
    planes[4] = r3;        // Near
    planes[5] = r4 - r3;   // Far

    for (int i = 0; i < 6; ++i) {
        float len = glm::length(float3(planes[i]));
        planes[i] /= len;
    }
}

// ============================================================
// Initialize
// ============================================================

bool GPUCulling::Initialize(rhi::IRHIDevice* device) {
    // ── 单阶段/Phase 1 PSO ──
    m_CS.stage      = rhi::ShaderStage::Compute;
    m_CS.spirv      = k_GPUCull_comp_spv;
    m_CS.entryPoint = "main";

    rhi::DescriptorSetLayoutDesc layoutDesc;
    layoutDesc.bindings = {
        {0, rhi::DescriptorType::StorageBuffer, 1, 32},
        {1, rhi::DescriptorType::StorageBuffer, 1, 32},
        {2, rhi::DescriptorType::StorageBuffer, 1, 32},
        {3, rhi::DescriptorType::CombinedImageSampler, 1, 32},
    };
    m_DescLayout = device->CreateDescriptorSetLayout(layoutDesc);
    m_DescSet    = device->AllocateDescriptorSet(m_DescLayout);

    // Single-phase: IndirectDraw + DrawCount SSBO
    // IndirectDrawCommand 在 MeshBatcher.h 定义为 20B（匹配 VkDrawIndexedIndirectCommand）
    {
        rhi::BufferDesc d; d.size = sizeof(IndirectDrawCommand) * kMaxObjects;
        d.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect; d.cpuAccess = true;
        m_IndirectCmdBuf = device->CreateBuffer(d);
        device->UpdateDescriptorSet(m_DescSet, 1, rhi::DescriptorType::StorageBuffer, m_IndirectCmdBuf.get());
    }
    {
        rhi::BufferDesc d; d.size = sizeof(u32) * 4;
        d.usage = rhi::BufferUsage::Storage; d.cpuAccess = true;
        m_DrawCountBuf = device->CreateBuffer(d);
        // 零初始化：首帧 Readback 读到 count=0 → 安全回退 CPU 路径
        u32* pCount = static_cast<u32*>(m_DrawCountBuf->Map());
        if (pCount) { *pCount = 0; m_DrawCountBuf->Unmap(); }
        device->UpdateDescriptorSet(m_DescSet, 2, rhi::DescriptorType::StorageBuffer, m_DrawCountBuf.get());
    }

    // Push constants: 6 planes + float4x4 VP + float2 screen + 2 uint
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

    // ── Phase 1 PSO（相同布局，不同 shader，输出候选索引）──
    m_Phase1CS.stage = rhi::ShaderStage::Compute;
    m_Phase1CS.spirv = k_GPUCull_Phase1_comp_spv;
    m_Phase1CS.entryPoint = "main";

    m_Phase1Layout = device->CreateDescriptorSetLayout(layoutDesc);
    m_Phase1Set    = device->AllocateDescriptorSet(m_Phase1Layout);

    rhi::PipelineStateDesc p1Desc;
    p1Desc.bindPoint = rhi::PipelineBindPoint::Compute;
    p1Desc.computeShader = &m_Phase1CS;
    p1Desc.pushConstantRanges = {pcRange};
    p1Desc.descriptorSetLayouts = {m_Phase1Layout};
    p1Desc.debugName = "GPUCull_Phase1";
    m_Phase1PSO = device->CreatePipelineState(p1Desc);
    HE_ASSERT(m_Phase1PSO, "GPUCulling: Phase 1 PSO creation failed");

    // Phase 1 候选缓冲
    {
        rhi::BufferDesc d; d.size = sizeof(u32) * kMaxObjects;
        d.usage = rhi::BufferUsage::Storage; d.cpuAccess = true;
        m_CandidateBuf = device->CreateBuffer(d);
        device->UpdateDescriptorSet(m_Phase1Set, 1, rhi::DescriptorType::StorageBuffer, m_CandidateBuf.get());
    }
    {
        rhi::BufferDesc d; d.size = sizeof(u32) * 4;
        d.usage = rhi::BufferUsage::Storage; d.cpuAccess = true;
        m_CandidateCountBuf = device->CreateBuffer(d);
        device->UpdateDescriptorSet(m_Phase1Set, 2, rhi::DescriptorType::StorageBuffer, m_CandidateCountBuf.get());
    }

    // ── Phase 2 PSO（5 个 binding）──
    m_Phase2CS.stage = rhi::ShaderStage::Compute;
    m_Phase2CS.spirv = k_GPUCull_TwoPhase_comp_spv;
    m_Phase2CS.entryPoint = "main";

    rhi::DescriptorSetLayoutDesc p2Layout;
    p2Layout.bindings = {
        {0, rhi::DescriptorType::StorageBuffer, 1, 32},        // u_SceneObjects
        {1, rhi::DescriptorType::StorageBuffer, 1, 32},        // u_Candidates
        {2, rhi::DescriptorType::StorageBuffer, 1, 32},        // u_DrawCount
        {3, rhi::DescriptorType::CombinedImageSampler, 1, 32}, // u_HiZ
        {4, rhi::DescriptorType::StorageBuffer, 1, 32},        // u_IndirectCmds
    };
    m_Phase2Layout = device->CreateDescriptorSetLayout(p2Layout);
    m_Phase2Set    = device->AllocateDescriptorSet(m_Phase2Layout);

    // Phase 2 固定绑定（不变的生命周期）
    device->UpdateDescriptorSet(m_Phase2Set, 1, rhi::DescriptorType::StorageBuffer, m_CandidateBuf.get());
    device->UpdateDescriptorSet(m_Phase2Set, 2, rhi::DescriptorType::StorageBuffer, m_DrawCountBuf.get());
    device->UpdateDescriptorSet(m_Phase2Set, 4, rhi::DescriptorType::StorageBuffer, m_IndirectCmdBuf.get());
    // binding 0 和 3 由 SetSceneBuffer / SetDepthTexture 设置

    // Phase 2 push constants: float4x4 vp + float2 screenSize + uint mips + uint count
    rhi::PushConstantRange p2PCR;
    p2PCR.stageMask = 32;
    p2PCR.offset = 0;
    p2PCR.size   = sizeof(float4x4) + sizeof(float2) + sizeof(u32) * 2;  // 64+8+8=80

    rhi::PipelineStateDesc p2Desc;
    p2Desc.bindPoint = rhi::PipelineBindPoint::Compute;
    p2Desc.computeShader = &m_Phase2CS;
    p2Desc.pushConstantRanges = {p2PCR};
    p2Desc.descriptorSetLayouts = {m_Phase2Layout};
    p2Desc.debugName = "GPUCull_Phase2";
    m_Phase2PSO = device->CreatePipelineState(p2Desc);
    HE_ASSERT(m_Phase2PSO, "GPUCulling: Phase 2 PSO creation failed");

    // ── Hi-Z 下采样 PSO ──
    {
        rhi::DescriptorSetLayoutDesc hizLayout;
        hizLayout.bindings = {{0, rhi::DescriptorType::CombinedImageSampler, 1, 32},
                              {1, rhi::DescriptorType::StorageImage, 1, 32}};
        m_HiZLayout = device->CreateDescriptorSetLayout(hizLayout);
        m_HiZSet    = device->AllocateDescriptorSet(m_HiZLayout);

        rhi::ShaderBytecode hizCS;
        hizCS.stage = rhi::ShaderStage::Compute;
        hizCS.spirv = k_HiZDownsample_comp_spv;
        hizCS.entryPoint = "main";

        rhi::PushConstantRange hizPCR;
        hizPCR.stageMask = 32;
        hizPCR.offset = 0;
        hizPCR.size   = 24;  // uint2 srcSize + uint2 dstSize + uint srcMip + uint _pad

        rhi::PipelineStateDesc hizDesc;
        hizDesc.bindPoint = rhi::PipelineBindPoint::Compute;
        hizDesc.computeShader = &hizCS;
        hizDesc.pushConstantRanges = {hizPCR};
        hizDesc.descriptorSetLayouts = {m_HiZLayout};
        hizDesc.debugName = "HiZDownsample";
        m_HiZ_PSO = device->CreatePipelineState(hizDesc);
        HE_ASSERT(m_HiZ_PSO, "GPUCulling: HiZDownsample PSO creation failed");
    }

    // Hi-Z 采样器
    {
        rhi::SamplerDesc sd;
        sd.minFilter = sd.magFilter = rhi::FilterMode::Nearest;
        sd.addressU = sd.addressV = rhi::AddressMode::ClampToEdge;
        sd.minLod = 0;
        sd.maxLod = (float)kHiZMips;
        m_HiZSampler = device->CreateSampler(sd);
    }

    m_Device = device;
    m_Initialized = true;
    HE_CORE_INFO("GPUCulling initialized (max {} objects, two-phase={})", kMaxObjects, useTwoPhase ? "on" : "off");
    return true;
}

// ============================================================
// Shutdown
// ============================================================

void GPUCulling::Shutdown(rhi::IRHIDevice* device) {
    if (!m_Initialized) return;

    // 清理 Hi-Z 每 mip 视图
    for (u32 i = 0; i < kHiZMips; ++i) {
        if (m_MipViews[i].storageView) {
            device->DestroyTextureMipView(m_MipViews[i].storageView);
            m_MipViews[i].storageView = nullptr;
        }
        if (m_MipViews[i].sampledView) {
            device->DestroyTextureMipView(m_MipViews[i].sampledView);
            m_MipViews[i].sampledView = nullptr;
        }
    }

    // 确保 PTG 线程已退出
    if (m_PTGActive) {
        ShutdownPTG(device);
    }

    m_CandidateBuf.reset();
    m_CandidateCountBuf.reset();
    m_Phase1PSO.reset();
    m_Phase2PSO.reset();
    m_IndirectCmdBuf.reset();
    m_DrawCountBuf.reset();
    m_PSO.reset();
    m_HiZTexture.reset();
    m_HiZSampler.reset();
    m_HiZ_PSO.reset();

    // PTG 资源清理
    m_PTG_PSO.reset();
    m_PTGParamBuf.reset();
    if (m_PTGLayout != rhi::kInvalidLayout && device) {
        device->DestroyDescriptorSetLayout(m_PTGLayout);
        m_PTGLayout = rhi::kInvalidLayout;
    }
    m_PTGSet = rhi::kInvalidSet;

    if (m_DescLayout != rhi::kInvalidLayout && device) {
        device->DestroyDescriptorSetLayout(m_DescLayout);
        m_DescLayout = rhi::kInvalidLayout;
    }
    if (m_Phase1Layout != rhi::kInvalidLayout && device) {
        device->DestroyDescriptorSetLayout(m_Phase1Layout);
        m_Phase1Layout = rhi::kInvalidLayout;
    }
    if (m_Phase2Layout != rhi::kInvalidLayout && device) {
        device->DestroyDescriptorSetLayout(m_Phase2Layout);
        m_Phase2Layout = rhi::kInvalidLayout;
    }
    if (m_HiZLayout != rhi::kInvalidLayout && device) {
        device->DestroyDescriptorSetLayout(m_HiZLayout);
        m_HiZLayout = rhi::kInvalidLayout;
    }
    m_DescSet    = rhi::kInvalidSet;
    m_Phase1Set  = rhi::kInvalidSet;
    m_Phase2Set  = rhi::kInvalidSet;
    m_PTGSet     = rhi::kInvalidSet;
    m_HiZSet     = rhi::kInvalidSet;
    m_Initialized = false;
}

// ============================================================
// SetSceneBuffer / SetDepthTexture
// ============================================================

void GPUCulling::SetSceneBuffer(rhi::IRHIDevice* device, rhi::IRHIBuffer* gpuSceneSSBO) {
    if (!m_Initialized || !gpuSceneSSBO) return;
    device->UpdateDescriptorSet(m_DescSet,   0, rhi::DescriptorType::StorageBuffer, gpuSceneSSBO);
    device->UpdateDescriptorSet(m_Phase1Set, 0, rhi::DescriptorType::StorageBuffer, gpuSceneSSBO);
    device->UpdateDescriptorSet(m_Phase2Set, 0, rhi::DescriptorType::StorageBuffer, gpuSceneSSBO);
    if (m_PTGActive) {
        device->UpdateDescriptorSet(m_PTGSet, 1, rhi::DescriptorType::StorageBuffer, gpuSceneSSBO);
    }
}

void GPUCulling::SetDepthTexture(rhi::IRHIDevice* device, rhi::IRHITexture* depthTex,
                                   u32 width, u32 height) {
    if (!m_Initialized) return;
    m_DepthInput = depthTex;
    m_HiZMipCount = 1;  // 当前仅使用全分辨率深度

    // 单阶段/Phase 1: 绑定上帧深度
    device->UpdateDescriptorSet(m_DescSet,   3, rhi::DescriptorType::CombinedImageSampler,
                                depthTex, m_HiZSampler.get());
    device->UpdateDescriptorSet(m_Phase1Set, 3, rhi::DescriptorType::CombinedImageSampler,
                                depthTex, m_HiZSampler.get());

    // Phase 2: 绑定当前帧深度（GBuffer 之后由外部更新）
    device->UpdateDescriptorSet(m_Phase2Set, 3, rhi::DescriptorType::CombinedImageSampler,
                                depthTex, m_HiZSampler.get());

    // PTG: 绑定深度纹理（与单阶段共用上帧深度）
    if (m_PTGActive) {
        device->UpdateDescriptorSet(m_PTGSet, 2, rhi::DescriptorType::CombinedImageSampler,
                                    depthTex, m_HiZSampler.get());
    }

    // ── 创建/重建 Hi-Z 纹理 ──
    if (!m_HiZTexture || m_HiZTexture->GetWidth() != width || m_HiZTexture->GetHeight() != height) {
        m_HiZTexture.reset();

        rhi::TextureDesc hizDesc;
        hizDesc.format = rhi::Format::R32_FLOAT;
        hizDesc.width  = width;
        hizDesc.height = height;
        hizDesc.mipLevels = kHiZMips;
        hizDesc.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::UnorderedAccess;
        m_HiZTexture = device->CreateTexture(hizDesc);
        HE_CORE_INFO("GPUCulling: Hi-Z texture created ({}x{}×{} mips)", width, height, kHiZMips);

        // 创建每 mip 的 storage/sampled 视图（供未来金字塔构建使用）
        for (u32 i = 0; i < kHiZMips; ++i) {
            if (m_MipViews[i].storageView) {
                device->DestroyTextureMipView(m_MipViews[i].storageView);
                m_MipViews[i].storageView = nullptr;
            }
            if (m_MipViews[i].sampledView) {
                device->DestroyTextureMipView(m_MipViews[i].sampledView);
                m_MipViews[i].sampledView = nullptr;
            }
            m_MipViews[i].storageView = device->CreateTextureMipStorageView(m_HiZTexture.get(), i);
            m_MipViews[i].sampledView = device->CreateTextureMipSampledView(m_HiZTexture.get(), i);
        }
    }
}

// ============================================================
// 单阶段 Dispatch
// ============================================================

void GPUCulling::Dispatch(rhi::IRHICommandList* cmd,
                           const float4x4& viewProj, u32 objectCount,
                           u32 screenW, u32 screenH) {
    if (!m_Initialized || !enabled || objectCount == 0) return;

    m_LastViewProj = viewProj;

    float4 planes[6];
    ExtractFrustumPlanes(viewProj, planes);

    struct { float4 planes[6]; float4x4 vp; float2 sSize; u32 mips; u32 count; } pc;
    for (int i = 0; i < 6; ++i) pc.planes[i] = planes[i];
    pc.vp    = viewProj;
    pc.sSize = float2((float)screenW, (float)screenH);
    // 首帧跳过 Hi-Z：上帧深度未初始化，全 0 深度会导致所有物体被遮挡剔除
    u32 hizMips = (m_FrameIndex == 0) ? 0 : m_HiZMipCount;
    m_FrameIndex++;

    pc.mips  = hizMips;
    pc.count = objectCount;

    cmd->SetPipeline(m_PSO.get());
    cmd->BindDescriptorSet(0, m_DescSet);
    cmd->SetPushConstants(0, sizeof(pc), &pc);

    u32 groups = (objectCount + 63) / 64;
    cmd->Dispatch(groups, 1, 1);
    // Dispatch 后清零 visibleCount, GBuffer 渲染回退 CPU 逐对象路径
    // GPU Culling 结果仅用于 Readback（统计/调试），不直接驱动 DrawIndexedIndirect
    m_LastVisibleCount = 0;
}

// ============================================================
// Phase 1: 粗筛 → 候选列表
// ============================================================

void GPUCulling::DispatchPhase1(rhi::IRHICommandList* cmd, const float4x4& viewProj,
                                 u32 objectCount, u32 screenW, u32 screenH) {
    if (!m_Initialized || !enabled || objectCount == 0) return;

    m_LastViewProj = viewProj;

    float4 planes[6];
    ExtractFrustumPlanes(viewProj, planes);

    struct { float4 planes[6]; float4x4 vp; float2 sSize; u32 mips; u32 count; } pc;
    for (int i = 0; i < 6; ++i) pc.planes[i] = planes[i];
    pc.vp    = viewProj;
    pc.sSize = float2((float)screenW, (float)screenH);
    u32 hizMips = (m_FrameIndex == 0) ? 0 : m_HiZMipCount; pc.mips = hizMips; m_FrameIndex++;
    pc.count = objectCount;

    // 清零候选计数
    void* countPtr = m_CandidateCountBuf->Map();
    if (countPtr) {
        std::memset(countPtr, 0, sizeof(u32) * 4);
        m_CandidateCountBuf->Unmap();
    }

    cmd->SetPipeline(m_Phase1PSO.get());
    cmd->BindDescriptorSet(0, m_Phase1Set);
    cmd->SetPushConstants(0, sizeof(pc), &pc);

    u32 groups = (objectCount + 63) / 64;
    cmd->Dispatch(groups, 1, 1);
}

// ============================================================
// BuildHiZPyramid — 构建 Hi-Z 深度金字塔
//
// 当前实现：深度缓冲可直接采样，m_HiZMipCount = 1。
// 后续在此处循环 Dispatch HiZDownsample 构建完整金字塔。
// ============================================================

void GPUCulling::BuildHiZPyramid(rhi::IRHICommandList* cmd, u32 screenW, u32 screenH) {
    if (!m_Initialized || !m_DepthInput || !m_HiZTexture) return;
    if (screenW < 2 || screenH < 2) { m_HiZMipCount = 1; return; }

    // 计算需要的 mip 级数（最小 1×1）
    u32 maxDim = (screenW > screenH) ? screenW : screenH;
    u32 mipCount = 1;
    while ((maxDim >> mipCount) >= 2) mipCount++;
    if (mipCount > kHiZMips) mipCount = kHiZMips;
    m_HiZMipCount = mipCount;

    cmd->SetPipeline(m_HiZ_PSO.get());

    u32 srcW = screenW, srcH = screenH;

    for (u32 mip = 0; mip < mipCount - 1; ++mip) {
        u32 dstW = srcW >> 1;
        u32 dstH = srcH >> 1;

        // ── 更新描述符 ──
        if (mip == 0) {
            // 第一级：从原始深度纹理下采样到 Hi-Z mip 1
            m_Device->UpdateDescriptorSet(m_HiZSet, 0,
                rhi::DescriptorType::CombinedImageSampler,
                m_DepthInput, m_HiZSampler.get());
        } else {
            // 后续级：从 Hi-Z mip N 下采样到 mip N+1
            m_Device->UpdateDescriptorSet(m_HiZSet, 0,
                rhi::DescriptorType::CombinedImageSampler,
                m_HiZTexture.get(), m_HiZSampler.get());
        }
        // 目标：Hi-Z mip N+1 的存储图像视图
        m_Device->UpdateDescriptorSetWithImageView(m_HiZSet, 1,
            rhi::DescriptorType::StorageImage,
            m_MipViews[mip + 1].storageView);

        cmd->BindDescriptorSet(0, m_HiZSet);

        // ── Push Constants ──
        struct { u32 srcW; u32 srcH; u32 dstW; u32 dstH; u32 srcMip; u32 _pad; } pc;
        pc.srcW   = srcW;
        pc.srcH   = srcH;
        pc.dstW   = dstW;
        pc.dstH   = dstH;
        pc.srcMip = mip;  // 源 mip 级别（深度纹理时 mip=0）
        pc._pad   = 0;
        cmd->SetPushConstants(0, sizeof(pc), &pc);

        // ── Dispatch ──
        u32 groupsX = (dstW + 15) / 16;
        u32 groupsY = (dstH + 15) / 16;
        cmd->Dispatch(groupsX, groupsY, 1);

        srcW = dstW;
        srcH = dstH;
    }

    // 金字塔构建完成后，将 Phase 2 的深度绑定切换为 Hi-Z 纹理
    m_Device->UpdateDescriptorSet(m_Phase2Set, 3,
        rhi::DescriptorType::CombinedImageSampler,
        m_HiZTexture.get(), m_HiZSampler.get());
}

// ============================================================
// Phase 2: 精筛 → IndirectDraw
// ============================================================

void GPUCulling::DispatchPhase2(rhi::IRHICommandList* cmd, u32 screenW, u32 screenH) {
    u32 candidateCount = GetPhase1CandidateCount();
    if (!m_Initialized || !enabled || candidateCount == 0) return;

    // 清零 DrawCount
    void* dcPtr = m_DrawCountBuf->Map();
    if (dcPtr) {
        std::memset(dcPtr, 0, sizeof(u32) * 4);
        m_DrawCountBuf->Unmap();
    }

    // Push constants
    struct { float4x4 vp; float2 sSize; u32 mips; u32 count; } pc;
    pc.vp    = m_LastViewProj;
    pc.sSize = float2((float)screenW, (float)screenH);
    u32 hizMips = (m_FrameIndex == 0) ? 0 : m_HiZMipCount; pc.mips = hizMips; m_FrameIndex++;
    pc.count = candidateCount;

    cmd->SetPipeline(m_Phase2PSO.get());
    cmd->BindDescriptorSet(0, m_Phase2Set);
    cmd->SetPushConstants(0, sizeof(pc), &pc);

    u32 groups = (candidateCount + 63) / 64;
    cmd->Dispatch(groups, 1, 1);

    // Phase 2 完成后，最终可见物体数需要从 m_DrawCountBuf 读回
    m_LastVisibleCount = 0;  // 延迟到 Readback 时更新
}

// ============================================================
// GetPhase1CandidateCount
// ============================================================

u32 GPUCulling::GetPhase1CandidateCount() const {
    if (!m_Initialized || !m_CandidateCountBuf) return 0;
    void* ptr = m_CandidateCountBuf->Map();
    if (!ptr) return 0;
    u32 count = *static_cast<const u32*>(ptr);
    m_CandidateCountBuf->Unmap();
    return count;
}

// ============================================================
// Readback
// ============================================================

void GPUCulling::Readback(rhi::IRHIDevice* device,
                           std::vector<u32>& outVisibleIndices) {
    (void)device;
    outVisibleIndices.clear();
    if (!m_Initialized || !enabled) return;

    u32 count = 0;
    void* dc = m_DrawCountBuf->Map();
    if (dc) { count = *static_cast<u32*>(dc); m_DrawCountBuf->Unmap(); }

    if (count == 0) { m_LastVisibleCount = 0; return; }
    if (count > kMaxObjects) count = kMaxObjects;

    outVisibleIndices.resize(count);
    void* vi = m_IndirectCmdBuf->Map();
    if (vi) {
        auto* cmds = static_cast<IndirectDrawCommand*>(vi);
        for (u32 i = 0; i < count; ++i)
            outVisibleIndices[i] = cmds[i].firstInstance;
        m_IndirectCmdBuf->Unmap();
    }

    m_LastVisibleCount = count;
}

// ============================================================
// PTG 初始化：创建信号缓冲 + PTG PSO，一次性 Dispatch
// ============================================================

bool GPUCulling::InitializePTG(rhi::IRHIDevice* device) {
    if (!m_Initialized || !device) return false;

    // 1. 创建 PTG 参数缓冲（CPU 每帧写入视锥参数 + 帧信号）
    {
        rhi::BufferDesc paramDesc;
        paramDesc.size = sizeof(PTGParams);  // 192 字节
        paramDesc.usage = rhi::BufferUsage::Uniform;
        paramDesc.cpuAccess = true;  // 允许 CPU Map/Unmap
        m_PTGParamBuf = device->CreateBuffer(paramDesc);

        // 清零初始化，确保 frameIndex = 0（PTG 等待帧索引 >= 1）
        void* ptr = m_PTGParamBuf->Map();
        if (ptr) {
            std::memset(ptr, 0, sizeof(PTGParams));
            m_PTGParamBuf->Unmap();
        }
    }

    // 2. 加载 PTG Shader
    m_PTGCS.stage = rhi::ShaderStage::Compute;
    m_PTGCS.spirv = k_PersistentCull_comp_spv;
    m_PTGCS.entryPoint = "main";

    // 3. 创建 PTG 描述符布局（5 个 binding）
    rhi::DescriptorSetLayoutDesc ptgLayout;
    ptgLayout.bindings = {
        {0, rhi::DescriptorType::UniformBuffer, 1, 32},        // u_PTGParams
        {1, rhi::DescriptorType::StorageBuffer, 1, 32},        // u_SceneObjects
        {2, rhi::DescriptorType::CombinedImageSampler, 1, 32}, // u_HiZDepth + Sampler
        {3, rhi::DescriptorType::StorageBuffer, 1, 32},        // u_IndirectCommands
        {4, rhi::DescriptorType::StorageBuffer, 1, 32},        // u_DrawCount
    };
    m_PTGLayout = device->CreateDescriptorSetLayout(ptgLayout);
    m_PTGSet = device->AllocateDescriptorSet(m_PTGLayout);

    // 4. 绑定 PTG 参数缓冲（binding 0）
    device->UpdateDescriptorSet(m_PTGSet, 0, rhi::DescriptorType::UniformBuffer, m_PTGParamBuf.get());
    // binding 1 (SceneObjects) 由 SetSceneBuffer 设置
    // binding 2 (HiZDepth) 由 SetDepthTexture 设置
    // binding 3 (IndirectCmds) 由 Initialize 中 m_IndirectCmdBuf 设置
    device->UpdateDescriptorSet(m_PTGSet, 3, rhi::DescriptorType::StorageBuffer, m_IndirectCmdBuf.get());
    // binding 4 (DrawCount) 由 Initialize 中 m_DrawCountBuf 设置
    device->UpdateDescriptorSet(m_PTGSet, 4, rhi::DescriptorType::StorageBuffer, m_DrawCountBuf.get());

    // 5. 创建 PTG PSO（无 push constants，全部通过描述符传递）
    rhi::PipelineStateDesc ptgDesc;
    ptgDesc.bindPoint = rhi::PipelineBindPoint::Compute;
    ptgDesc.computeShader = &m_PTGCS;
    ptgDesc.pushConstantRanges = {};
    ptgDesc.descriptorSetLayouts = {m_PTGLayout};
    ptgDesc.debugName = "PersistentCull";
    m_PTG_PSO = device->CreatePipelineState(ptgDesc);
    HE_ASSERT(m_PTG_PSO, "GPUCulling: PersistentCull PSO creation failed");

    // 6. PTG per-frame dispatch 模式：无需初始 Dispatch
    //   每帧由 SignalPTG 执行 vkCmdDispatch（通过 AsyncCompute 队列）

    m_PTGActive = true;
    m_PTGFrameCounter = 1;  // 帧计数器从 1 开始，0 为初始空闲态
    HE_CORE_INFO("GPUCulling: PTG initialized ({} groups, {} threads total)",
                 kPTGGroupCount, kPTGGroupCount * 64);
    return true;
}

// ============================================================
// SignalPTG — 每帧 Dispatch PTG Compute Shader
// 当前为 per-frame dispatch 模式（非持久化），用于验证 shader/描述符正确性。
// 确认白屏问题解决后再切换为真正的持久化 spin-wait 模式。
// ============================================================

void GPUCulling::SignalPTG(rhi::IRHICommandList* cmd, const float4x4& viewProj,
                            u32 objectCount, u32 screenW, u32 screenH) {
    if (!m_PTGActive) return;

    m_LastViewProj = viewProj;

    // 提取 6 个视锥平面
    float4 planes[6];
    ExtractFrustumPlanes(viewProj, planes);

    // 写入 PTGParams 到 Uniform Buffer
    void* mapped = m_PTGParamBuf->Map();
    if (!mapped) return;

    auto* params = static_cast<PTGParams*>(mapped);
    params->viewProj = viewProj;
    for (int i = 0; i < 6; ++i) params->frustumPlanes[i] = planes[i];
    params->screenSize = float2((float)screenW, (float)screenH);
    params->objectCount = objectCount;
    params->hizMipCount = (m_FrameIndex == 0) ? 0 : m_HiZMipCount; m_FrameIndex++;
    params->frameIndex = m_PTGFrameCounter++;

    m_PTGParamBuf->Unmap();

    // 每帧 Dispatch PTG shader（替代持久化 spin-wait）
    cmd->SetPipeline(m_PTG_PSO.get());
    cmd->BindDescriptorSet(0, m_PTGSet);
    cmd->Dispatch(kPTGGroupCount, 1, 1);

    // 全局屏障：确保 Compute Shader 写入对后续 Indirect Draw 可见
    cmd->PipelineBarrier(
        rhi::PipelineStage::ComputeShader, rhi::PipelineStage::DrawIndirect,
        rhi::ResourceState::UnorderedAccess, rhi::ResourceState::IndirectArgument);
    // PTG Signal 后清零, GBuffer 回退 CPU 逐对象路径
    m_LastVisibleCount = 0;
}

// ============================================================
// ShutdownPTG — 发送退出信号并等待 PTG 线程终止
// ============================================================

void GPUCulling::ShutdownPTG(rhi::IRHIDevice* device) {
    if (!m_PTGActive || !device) return;

    // 1. 发送退出信号（frameIndex = 0xFFFFFFFF）
    void* mapped = m_PTGParamBuf->Map();
    if (mapped) {
        auto* params = static_cast<PTGParams*>(mapped);
        params->frameIndex = 0xFFFFFFFF;
        m_PTGParamBuf->Unmap();
    }

    // 2. 等待 GPU 完成，确保 PTG 线程检测到退出信号并终止
    device->WaitIdle();

    m_PTGActive = false;
    m_PTGFrameCounter = 0;
    HE_CORE_INFO("GPUCulling: PTG shutdown complete");
}

} // namespace he::render
