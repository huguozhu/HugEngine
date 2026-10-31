#include "Pipeline/GPUCulling.h"
#include <iterator>
#include "Math/Geometry.h"   // 统一视锥提取（he::Frustum::FromViewProj）
#include "Pipeline/MeshBatcher.h"  // IndirectDrawCommand
#include "GPUCull.comp.spv.h"
#include "GPUCull.comp.dxil.h"
#include "GPUCull_Phase1.comp.spv.h"
#include "GPUCull_Phase1.comp.dxil.h"
#include "GPUCull_TwoPhase.comp.spv.h"
#include "GPUCull_TwoPhase.comp.dxil.h"
#include "HiZDownsample.comp.spv.h"
#include "HiZDownsample.comp.dxil.h"
#include "PersistentCull.comp.spv.h"
#include "PersistentCull.comp.dxil.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>   // 【T7 诊断】std::getenv（HE_T7_DEBUG）
#include <glm/gtc/matrix_access.hpp>

// ============================================================
// GPUCulling 实现 — 单阶段/两阶段遮挡剔除
// ============================================================

namespace he::render {

// ============================================================
// GPUCulling 各描述符集绑定号（与对应 compute shader 一致）
// ============================================================
// 主剔除集（m_DescSet）
static constexpr u32 kCullBindSceneObjects = 0;   // GPUObjectData[] SSBO
static constexpr u32 kCullBindIndirectCmds = 1;   // IndirectDraw 命令
static constexpr u32 kCullBindDrawCount    = 2;   // 绘制计数
static constexpr u32 kCullBindDepth        = 3;   // 深度缓冲（Hi-Z 来源）

// Phase 1 粗筛集（m_Phase1Set）
static constexpr u32 kPhase1BindSceneObjects   = 0;
static constexpr u32 kPhase1BindCandidates     = 1;   // 候选索引
static constexpr u32 kPhase1BindCandidateCount = 2;   // 候选计数

// Phase 2 精筛集（m_Phase2Set）
static constexpr u32 kPhase2BindSceneObjects = 0;
static constexpr u32 kPhase2BindCandidates   = 1;
static constexpr u32 kPhase2BindDrawCount    = 2;
static constexpr u32 kPhase2BindHiZ          = 3;   // Hi-Z 金字塔
static constexpr u32 kPhase2BindIndirectCmds = 4;

// Hi-Z 下采样集（m_HiZSet）
static constexpr u32 kHiZBindSrcDepth = 0;   // 源深度
static constexpr u32 kHiZBindDstDepth = 1;   // 目标 mip（StorageImage）

// 持久化线程组集（m_PTGSet）
static constexpr u32 kPTGBindSceneObjects = 1;

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


// ============================================================
// Initialize
// ============================================================

bool GPUCulling::Initialize(rhi::IRHIDevice* device) {
    // A-3：句柄解析依赖设备指针 —— 原来要到函数末尾（单相初始化之后）才赋值，
    // 早段创建缓冲后立刻 ResolveBuffer 会拿到 nullptr（这正是上一批 Initialize 崩溃的原因）。
    m_Device = device;
    // ── 单阶段/Phase 1 PSO ──
    m_CS.stage      = rhi::ShaderStage::Compute;
    m_CS.spirv      = k_GPUCull_comp_spv;
    m_CS.dxil.assign(std::begin(k_GPUCull_comp_dxil), std::end(k_GPUCull_comp_dxil));
    m_CS.dxil.assign(std::begin(k_GPUCull_comp_dxil), std::end(k_GPUCull_comp_dxil));
    m_CS.dxil.assign(std::begin(k_GPUCull_comp_dxil), std::end(k_GPUCull_comp_dxil));
    m_CS.entryPoint = "main";

    rhi::DescriptorSetLayoutDesc layoutDesc;
    layoutDesc.bindings = {
        {kCullBindSceneObjects, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},
        {kCullBindIndirectCmds, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},
        {kCullBindDrawCount,    rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},
        {kCullBindDepth,        rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute},
    };
    m_DescLayout = device->CreateDescriptorSetLayout(layoutDesc);
    m_DescSet    = device->AllocateDescriptorSet(m_DescLayout);

    // Single-phase: IndirectDraw + DrawCount SSBO
    // IndirectDrawCommand 在 MeshBatcher.h 定义为 20B（匹配 VkDrawIndexedIndirectCommand）
    {
        rhi::BufferDesc d;
        d.size = sizeof(IndirectDrawCommand) * kMaxObjects;
        d.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect;
        d.cpuAccess = true;
        m_IndirectCmdBuf = device->CreateBufferHandle(d);   // A-3
        HE_ASSERT(m_IndirectCmdBuf.IsValid(), "GPUCulling: 间接命令缓冲句柄创建失败");
        device->UpdateDescriptorSet(m_DescSet, kCullBindIndirectCmds, rhi::DescriptorType::StorageBuffer, ResB(m_IndirectCmdBuf));
    }
    {
        rhi::BufferDesc d;
        d.size = sizeof(u32) * 4;
        // 【T7】计数缓冲同时是 `DrawIndexedIndirectCount` 的 countBuffer：
        // Vulkan 要求 countBuffer 必须带 INDIRECT_BUFFER usage（VUID-vkCmdDrawIndexedIndirectCount-countBuffer-02715），
        // 此前只声明 Storage ⇒ 一旦真正用 GPU 计数驱动绘制，校验层每帧报警。D3D12 侧该标志无副作用。
        d.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect;
        d.cpuAccess = true;
        m_DrawCountBuf = device->CreateBufferHandle(d);   // A-3
        HE_ASSERT(m_DrawCountBuf.IsValid(), "GPUCulling: 绘制计数缓冲句柄创建失败");
        // 零初始化：首帧 Readback 读到 count=0 → 安全回退 CPU 路径
        auto* dcB    = ResB(m_DrawCountBuf);
        HE_ASSERT(dcB, "GPUCulling: 绘制计数缓冲句柄解析失败");
        u32*  pCount = dcB ? static_cast<u32*>(dcB->Map()) : nullptr;
        if (pCount) { *pCount = 0; if (auto* ub = ResB(m_DrawCountBuf)) ub->Unmap(); }
        device->UpdateDescriptorSet(m_DescSet, kCullBindDrawCount, rhi::DescriptorType::StorageBuffer, ResB(m_DrawCountBuf));
    }

    // Push constants: 6 planes + float4x4 VP + float2 screen + 2 uint
    rhi::PushConstantRange pcRange;
    pcRange.stageMask = rhi::kStageMaskCompute;
    pcRange.offset = 0;
    pcRange.size   = sizeof(float4) * 6 + sizeof(float4x4) + sizeof(float2) + sizeof(u32) * 4;

    rhi::PipelineStateDesc psoDesc;
    psoDesc.bindPoint = rhi::PipelineBindPoint::Compute;
    psoDesc.computeShader = &m_CS;
    psoDesc.pushConstantRanges = {pcRange};
    psoDesc.descriptorSetLayouts = {m_DescLayout};
    psoDesc.debugName = "GPUCull";

    m_PSO = device->CreatePipelineStateHandle(psoDesc);
    HE_ASSERT(m_PSO.IsValid(), "GPUCulling: Compute PSO creation failed");

    // ── Phase 1 PSO（相同布局，不同 shader，输出候选索引）──
    m_Phase1CS.stage = rhi::ShaderStage::Compute;
    m_Phase1CS.spirv = k_GPUCull_Phase1_comp_spv;
    m_Phase1CS.dxil.assign(std::begin(k_GPUCull_Phase1_comp_dxil), std::end(k_GPUCull_Phase1_comp_dxil));
    m_Phase1CS.dxil.assign(std::begin(k_GPUCull_Phase1_comp_dxil), std::end(k_GPUCull_Phase1_comp_dxil));
    m_Phase1CS.dxil.assign(std::begin(k_GPUCull_Phase1_comp_dxil), std::end(k_GPUCull_Phase1_comp_dxil));
    m_Phase1CS.entryPoint = "main";

    m_Phase1Layout = device->CreateDescriptorSetLayout(layoutDesc);
    m_Phase1Set    = device->AllocateDescriptorSet(m_Phase1Layout);

    rhi::PipelineStateDesc p1Desc;
    p1Desc.bindPoint = rhi::PipelineBindPoint::Compute;
    p1Desc.computeShader = &m_Phase1CS;
    p1Desc.pushConstantRanges = {pcRange};
    p1Desc.descriptorSetLayouts = {m_Phase1Layout};
    p1Desc.debugName = "GPUCull_Phase1";
    m_Phase1PSO = device->CreatePipelineStateHandle(p1Desc);
    HE_ASSERT(m_Phase1PSO.IsValid(), "GPUCulling: Phase 1 PSO creation failed");

    // Phase 1 候选缓冲
    {
        rhi::BufferDesc d;
        d.size = sizeof(u32) * kMaxObjects;
        d.usage = rhi::BufferUsage::Storage;
        d.cpuAccess = true;
        m_CandidateBuf = device->CreateBufferHandle(d);   // A-3
        HE_ASSERT(m_CandidateBuf.IsValid(), "GPUCulling: 候选列表缓冲句柄创建失败");
        device->UpdateDescriptorSet(m_Phase1Set, kPhase1BindCandidates, rhi::DescriptorType::StorageBuffer, ResB(m_CandidateBuf));
    }
    {
        rhi::BufferDesc d;
        d.size = sizeof(u32) * 4;
        d.usage = rhi::BufferUsage::Storage;
        d.cpuAccess = true;
        m_CandidateCountBuf = device->CreateBufferHandle(d);   // A-3
        HE_ASSERT(m_CandidateCountBuf.IsValid(), "GPUCulling: 候选计数缓冲句柄创建失败");
        device->UpdateDescriptorSet(m_Phase1Set, kPhase1BindCandidateCount, rhi::DescriptorType::StorageBuffer, ResB(m_CandidateCountBuf));
    }

    // ── Phase 2 PSO（5 个 binding）──
    m_Phase2CS.stage = rhi::ShaderStage::Compute;
    m_Phase2CS.spirv = k_GPUCull_TwoPhase_comp_spv;
    m_Phase2CS.dxil.assign(std::begin(k_GPUCull_TwoPhase_comp_dxil), std::end(k_GPUCull_TwoPhase_comp_dxil));
    m_Phase2CS.dxil.assign(std::begin(k_GPUCull_TwoPhase_comp_dxil), std::end(k_GPUCull_TwoPhase_comp_dxil));
    m_Phase2CS.dxil.assign(std::begin(k_GPUCull_TwoPhase_comp_dxil), std::end(k_GPUCull_TwoPhase_comp_dxil));
    m_Phase2CS.entryPoint = "main";

    rhi::DescriptorSetLayoutDesc p2Layout;
    p2Layout.bindings = {
        {kPhase2BindSceneObjects, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},        // u_SceneObjects
        {kPhase2BindCandidates,   rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},        // u_Candidates
        {kPhase2BindDrawCount,    rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},        // u_DrawCount
        {kPhase2BindHiZ,          rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute}, // u_HiZ
        {kPhase2BindIndirectCmds, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},        // u_IndirectCmds
    };
    m_Phase2Layout = device->CreateDescriptorSetLayout(p2Layout);
    m_Phase2Set    = device->AllocateDescriptorSet(m_Phase2Layout);

    // Phase 2 固定绑定（不变的生命周期）
    device->UpdateDescriptorSet(m_Phase2Set, kPhase2BindCandidates, rhi::DescriptorType::StorageBuffer, ResB(m_CandidateBuf));
    device->UpdateDescriptorSet(m_Phase2Set, kPhase2BindDrawCount, rhi::DescriptorType::StorageBuffer, ResB(m_DrawCountBuf));
    device->UpdateDescriptorSet(m_Phase2Set, kPhase2BindIndirectCmds, rhi::DescriptorType::StorageBuffer, ResB(m_IndirectCmdBuf));
    // binding 0 和 3 由 SetSceneBuffer / SetDepthTexture 设置

    // Phase 2 push constants: float4x4 vp + float2 screenSize + uint mips + uint count
    rhi::PushConstantRange p2PCR;
    p2PCR.stageMask = rhi::kStageMaskCompute;
    p2PCR.offset = 0;
    p2PCR.size   = sizeof(float4x4) + sizeof(float2) + sizeof(u32) * 2;  // 64+8+8=80

    rhi::PipelineStateDesc p2Desc;
    p2Desc.bindPoint = rhi::PipelineBindPoint::Compute;
    p2Desc.computeShader = &m_Phase2CS;
    p2Desc.pushConstantRanges = {p2PCR};
    p2Desc.descriptorSetLayouts = {m_Phase2Layout};
    p2Desc.debugName = "GPUCull_Phase2";
    m_Phase2PSO = device->CreatePipelineStateHandle(p2Desc);
    HE_ASSERT(m_Phase2PSO.IsValid(), "GPUCulling: Phase 2 PSO creation failed");

    // ── Hi-Z 下采样 PSO ──
    {
        rhi::DescriptorSetLayoutDesc hizLayout;
        hizLayout.bindings = {{kHiZBindSrcDepth, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute},
                              {kHiZBindDstDepth, rhi::DescriptorType::StorageImage, 1, rhi::kStageMaskCompute}};
        m_HiZLayout = device->CreateDescriptorSetLayout(hizLayout);
        m_HiZSet    = device->AllocateDescriptorSet(m_HiZLayout);

        rhi::ShaderBytecode hizCS;
        hizCS.stage = rhi::ShaderStage::Compute;
        hizCS.spirv = k_HiZDownsample_comp_spv;
        hizCS.dxil.assign(std::begin(k_HiZDownsample_comp_dxil), std::end(k_HiZDownsample_comp_dxil));
        hizCS.dxil.assign(std::begin(k_HiZDownsample_comp_dxil), std::end(k_HiZDownsample_comp_dxil));
        hizCS.dxil.assign(std::begin(k_HiZDownsample_comp_dxil), std::end(k_HiZDownsample_comp_dxil));
        hizCS.entryPoint = "main";

        rhi::PushConstantRange hizPCR;
        hizPCR.stageMask = rhi::kStageMaskCompute;
        hizPCR.offset = 0;
        hizPCR.size   = 24;  // uint2 srcSize + uint2 dstSize + uint srcMip + uint _pad

        rhi::PipelineStateDesc hizDesc;
        hizDesc.bindPoint = rhi::PipelineBindPoint::Compute;
        hizDesc.computeShader = &hizCS;
        hizDesc.pushConstantRanges = {hizPCR};
        hizDesc.descriptorSetLayouts = {m_HiZLayout};
        hizDesc.debugName = "HiZDownsample";
        m_HiZ_PSO = device->CreatePipelineStateHandle(hizDesc);
        HE_ASSERT(m_HiZ_PSO.IsValid(), "GPUCulling: HiZDownsample PSO creation failed");
    }

    // Hi-Z 采样器
    {
        rhi::SamplerDesc sd;
        sd.minFilter = sd.magFilter = rhi::FilterMode::Nearest;
        sd.addressU = sd.addressV = rhi::AddressMode::ClampToEdge;
        sd.minLod = 0;
        sd.maxLod = (float)kHiZMips;
        m_HiZSampler = device->CreateSamplerHandle(sd);   // A-3
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

    m_Device->DestroyBufferHandle(m_CandidateBuf); m_CandidateBuf = {};
    m_Device->DestroyBufferHandle(m_CandidateCountBuf); m_CandidateCountBuf = {};
    m_Device->DestroyPipelineStateHandle(m_Phase1PSO); m_Phase1PSO = {};
    m_Device->DestroyPipelineStateHandle(m_Phase2PSO); m_Phase2PSO = {};
    m_Device->DestroyBufferHandle(m_IndirectCmdBuf); m_IndirectCmdBuf = {};
    m_Device->DestroyBufferHandle(m_DrawCountBuf); m_DrawCountBuf = {};
    m_Device->DestroyPipelineStateHandle(m_PSO); m_PSO = {};
    m_Device->DestroyTextureHandle(m_HiZTexture); m_HiZTexture = {};
    m_Device->DestroySamplerHandle(m_HiZSampler);
    m_HiZSampler = {};
    m_Device->DestroyPipelineStateHandle(m_HiZ_PSO); m_HiZ_PSO = {};

    // PTG 资源清理
    m_Device->DestroyPipelineStateHandle(m_PTG_PSO); m_PTG_PSO = {};
    m_Device->DestroyBufferHandle(m_PTGParamBuf); m_PTGParamBuf = {};
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
    device->UpdateDescriptorSet(m_DescSet, kCullBindSceneObjects, rhi::DescriptorType::StorageBuffer, gpuSceneSSBO);
    device->UpdateDescriptorSet(m_Phase1Set, kPhase1BindSceneObjects, rhi::DescriptorType::StorageBuffer, gpuSceneSSBO);
    device->UpdateDescriptorSet(m_Phase2Set, kPhase2BindSceneObjects, rhi::DescriptorType::StorageBuffer, gpuSceneSSBO);
    if (m_PTGActive) {
        device->UpdateDescriptorSet(m_PTGSet, kPTGBindSceneObjects, rhi::DescriptorType::StorageBuffer, gpuSceneSSBO);
    }
}

void GPUCulling::SetDepthTexture(rhi::IRHIDevice* device, rhi::IRHITexture* depthTex,
                                   u32 width, u32 height) {
    if (!m_Initialized) return;
    m_DepthInput = depthTex;
    // 【T7 诊断开关】HE_T7_NO_HIZ=1 时只做视锥剔除（hizMipCount=0）：
    // 单级 Hi-Z 的遮挡测试在保守方向之外仍可能误剔除，这个开关用于"关掉遮挡后画面是否恢复正常"的对照。
    static const bool s_NoHiZ = std::getenv("HE_T7_NO_HIZ") != nullptr;
    m_HiZMipCount = s_NoHiZ ? 0 : 1;  // 当前仅使用全分辨率深度

    // 单阶段/Phase 1: 绑定上帧深度
    device->UpdateDescriptorSet(m_DescSet, kCullBindDepth, rhi::DescriptorType::CombinedImageSampler,
                                depthTex, ResS(m_HiZSampler));
    device->UpdateDescriptorSet(m_Phase1Set, 3, rhi::DescriptorType::CombinedImageSampler,
                                depthTex, ResS(m_HiZSampler));

    // Phase 2: 绑定当前帧深度（GBuffer 之后由外部更新）
    device->UpdateDescriptorSet(m_Phase2Set, kPhase2BindHiZ, rhi::DescriptorType::CombinedImageSampler,
                                depthTex, ResS(m_HiZSampler));

    // PTG: 绑定深度纹理（与单阶段共用上帧深度）
    if (m_PTGActive) {
        device->UpdateDescriptorSet(m_PTGSet, 2, rhi::DescriptorType::CombinedImageSampler,
                                    depthTex, ResS(m_HiZSampler));
    }

    // ── 创建/重建 Hi-Z 纹理 ──
    // A-3：先判句柄有效（过期 ⇒ 视为需要重建），再比较尺寸
    if (!m_HiZTexture.IsValid() ||
        ResT(m_HiZTexture)->GetWidth()  != width ||
        ResT(m_HiZTexture)->GetHeight() != height) {
        m_Device->DestroyTextureHandle(m_HiZTexture);
        m_HiZTexture = {};

        rhi::TextureDesc hizDesc;
        hizDesc.format = rhi::Format::R32_FLOAT;
        hizDesc.width  = width;
        hizDesc.height = height;
        hizDesc.mipLevels = kHiZMips;
        hizDesc.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::UnorderedAccess;
        m_HiZTexture = device->CreateTextureHandle(hizDesc);   // A-3
        HE_ASSERT(m_HiZTexture.IsValid(), "GPUCulling: Hi-Z 纹理句柄创建失败");
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
            m_MipViews[i].storageView = device->CreateTextureMipStorageView(ResT(m_HiZTexture), i);
            m_MipViews[i].sampledView = device->CreateTextureMipSampledView(ResT(m_HiZTexture), i);
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
    he::Frustum::ToPlaneArray(he::Frustum::FromViewProj(viewProj), planes);

    struct { float4 planes[6]; float4x4 vp; float2 sSize; u32 mips; u32 count; } pc;
    for (int i = 0; i < 6; ++i) pc.planes[i] = planes[i];
    pc.vp    = viewProj;
    pc.sSize = float2((float)screenW, (float)screenH);
    // 首帧跳过 Hi-Z：上帧深度未初始化，全 0 深度会导致所有物体被遮挡剔除
    u32 hizMips = (m_FrameIndex == 0) ? 0 : m_HiZMipCount;
    m_FrameIndex++;

    pc.mips  = hizMips;
    pc.count = objectCount;

    cmd->SetPipeline(ResP(m_PSO));
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_DescSet);
    cmd->SetPushConstants(0, sizeof(pc), &pc);

    u32 groups = (objectCount + 63) / 64;
    // Dispatch 调试 marker（RenderDoc 定位用）
    char label[64];
    snprintf(label, sizeof(label), "Cull Single (%u obj)", objectCount);
    cmd->SetDrawDebugLabel(label);
    cmd->Dispatch(groups, 1, 1);
    // 【T7】这里**不再**把 m_LastVisibleCount 清零。本派发已经把可见物体的间接命令与计数
    // 直接写在 GPU 缓冲里，GBuffer 用 `DrawIndexedIndirectCount` 消费同一份计数（CPU 不参与
    // 条数决定）。m_LastVisibleCount 只保留上一帧 Readback 读回的统计值（界面/日志显示）。
}

// ============================================================
// Phase 1: 粗筛 → 候选列表
// ============================================================

void GPUCulling::DispatchPhase1(rhi::IRHICommandList* cmd, const float4x4& viewProj,
                                 u32 objectCount, u32 screenW, u32 screenH) {
    if (!m_Initialized || !enabled || objectCount == 0) return;

    m_LastViewProj = viewProj;

    float4 planes[6];
    he::Frustum::ToPlaneArray(he::Frustum::FromViewProj(viewProj), planes);

    struct { float4 planes[6]; float4x4 vp; float2 sSize; u32 mips; u32 count; } pc;
    for (int i = 0; i < 6; ++i) pc.planes[i] = planes[i];
    pc.vp    = viewProj;
    pc.sSize = float2((float)screenW, (float)screenH);
    u32 hizMips = (m_FrameIndex == 0) ? 0 : m_HiZMipCount;
    pc.mips = hizMips;
    m_FrameIndex++;
    pc.count = objectCount;

    // 清零候选计数
    auto* ccB      = ResB(m_CandidateCountBuf);
    HE_ASSERT(ccB, "GPUCulling: 候选计数缓冲句柄解析失败");
    void* countPtr = ccB ? ccB->Map() : nullptr;
    if (countPtr) {
        std::memset(countPtr, 0, sizeof(u32) * 4);
        if (auto* ub = ResB(m_CandidateCountBuf)) ub->Unmap();
    }

    cmd->SetPipeline(ResP(m_Phase1PSO));
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Phase1Set);
    cmd->SetPushConstants(0, sizeof(pc), &pc);

    u32 groups = (objectCount + 63) / 64;
    // Dispatch 调试 marker（RenderDoc 定位用）
    char label[64];
    snprintf(label, sizeof(label), "Cull Phase1 (%u obj)", objectCount);
    cmd->SetDrawDebugLabel(label);
    cmd->Dispatch(groups, 1, 1);
}

// ============================================================
// BuildHiZPyramid — 构建 Hi-Z 深度金字塔
//
// 当前实现：深度缓冲可直接采样，m_HiZMipCount = 1。
// 后续在此处循环 Dispatch HiZDownsample 构建完整金字塔。
// ============================================================

void GPUCulling::BuildHiZPyramid(rhi::IRHICommandList* cmd, u32 screenW, u32 screenH) {
    if (!m_Initialized || !m_DepthInput || !m_HiZTexture.IsValid()) return;
    if (screenW < 2 || screenH < 2) { m_HiZMipCount = 1; return; }

    // 计算需要的 mip 级数（最小 1×1）
    u32 maxDim = (screenW > screenH) ? screenW : screenH;
    u32 mipCount = 1;
    while ((maxDim >> mipCount) >= 2) mipCount++;
    if (mipCount > kHiZMips) mipCount = kHiZMips;
    m_HiZMipCount = mipCount;

    cmd->SetPipeline(ResP(m_HiZ_PSO));

    u32 srcW = screenW, srcH = screenH;

    for (u32 mip = 0; mip < mipCount - 1; ++mip) {
        u32 dstW = srcW >> 1;
        u32 dstH = srcH >> 1;

        // ── 更新描述符 ──
        if (mip == 0) {
            // 第一级：从原始深度纹理下采样到 Hi-Z mip 1
            m_Device->UpdateDescriptorSet(m_HiZSet, 0,
                rhi::DescriptorType::CombinedImageSampler,
                m_DepthInput, ResS(m_HiZSampler));
        } else {
            // 后续级：从 Hi-Z mip N 下采样到 mip N+1
            m_Device->UpdateDescriptorSet(m_HiZSet, 0,
                rhi::DescriptorType::CombinedImageSampler,
                ResT(m_HiZTexture), ResS(m_HiZSampler));
        }
        // 目标：Hi-Z mip N+1 的存储图像视图
        m_Device->UpdateDescriptorSetWithImageView(m_HiZSet, 1,
            rhi::DescriptorType::StorageImage,
            m_MipViews[mip + 1].storageView);

        cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_HiZSet);

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
        // Dispatch 调试 marker：标记当前 HiZ mip 级别（RenderDoc 定位用）
        char label[64];
        snprintf(label, sizeof(label), "Cull HiZ Mip%u", mip);
        cmd->SetDrawDebugLabel(label);
        cmd->Dispatch(groupsX, groupsY, 1);

        srcW = dstW;
        srcH = dstH;
    }

    // 金字塔构建完成后，将 Phase 2 的深度绑定切换为 Hi-Z 纹理
    m_Device->UpdateDescriptorSet(m_Phase2Set, kPhase2BindHiZ,
        rhi::DescriptorType::CombinedImageSampler,
        ResT(m_HiZTexture), ResS(m_HiZSampler));
}

// ============================================================
// Phase 2: 精筛 → IndirectDraw
// ============================================================

void GPUCulling::DispatchPhase2(rhi::IRHICommandList* cmd, u32 screenW, u32 screenH) {
    u32 candidateCount = GetPhase1CandidateCount();
    if (!m_Initialized || !enabled || candidateCount == 0) return;

    // 清零 DrawCount
    auto* dcB   = ResB(m_DrawCountBuf);
    HE_ASSERT(dcB, "GPUCulling: 绘制计数缓冲句柄解析失败");
    void* dcPtr = dcB ? dcB->Map() : nullptr;
    if (dcPtr) {
        std::memset(dcPtr, 0, sizeof(u32) * 4);
        if (auto* ub = ResB(m_DrawCountBuf)) ub->Unmap();
    }

    // Push constants
    struct { float4x4 vp; float2 sSize; u32 mips; u32 count; } pc;
    pc.vp    = m_LastViewProj;
    pc.sSize = float2((float)screenW, (float)screenH);
    u32 hizMips = (m_FrameIndex == 0) ? 0 : m_HiZMipCount;
    pc.mips = hizMips;
    m_FrameIndex++;
    pc.count = candidateCount;

    cmd->SetPipeline(ResP(m_Phase2PSO));
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Phase2Set);
    cmd->SetPushConstants(0, sizeof(pc), &pc);

    u32 groups = (candidateCount + 63) / 64;
    // Dispatch 调试 marker（RenderDoc 定位用）
    char label[64];
    snprintf(label, sizeof(label), "Cull Phase2 (%u cand)", candidateCount);
    cmd->SetDrawDebugLabel(label);
    cmd->Dispatch(groups, 1, 1);

    // 【T7】同 Dispatch：Phase 2 已把间接命令与计数写进 GPU 缓冲，绘制端直接消费该计数，
    // 因此不再清零 m_LastVisibleCount（它只作为上一帧 Readback 的统计值）。
}

// ============================================================
// GetPhase1CandidateCount
// ============================================================

u32 GPUCulling::GetPhase1CandidateCount() const {
    if (!m_Initialized || !m_CandidateCountBuf.IsValid()) return 0;
    auto* ccB = ResB(m_CandidateCountBuf);
    HE_ASSERT(ccB, "GPUCulling: 候选计数缓冲句柄解析失败");
    void* ptr = ccB ? ccB->Map() : nullptr;
    if (!ptr) return 0;
    u32 count = *static_cast<const u32*>(ptr);
    if (auto* ub = ResB(m_CandidateCountBuf)) ub->Unmap();
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
    auto* dcBb = ResB(m_DrawCountBuf);
    HE_ASSERT(dcBb, "GPUCulling: 绘制计数缓冲句柄解析失败");
    void* dc   = dcBb ? dcBb->Map() : nullptr;
    if (dc) { count = *static_cast<u32*>(dc); if (auto* ub = ResB(m_DrawCountBuf)) ub->Unmap(); }

    // 【T7 可观测性】这里读回的 count 是 GPU 剔除着色器写进计数缓冲的值（上一帧派发的结果），
    // 不是 CPU 剔除结果 —— 它是"GPU 驱动路径确实在跑、且可见数非 0"的直接证据。
    // 前两次（首帧 0 与第二帧的首个真实值）必打，之后每 120 帧打一次。
    {
        static u32 s_ReadbackTimes = 0;
        ++s_ReadbackTimes;
        if (s_ReadbackTimes <= 2u || s_ReadbackTimes % 120u == 0u) {
            HE_CORE_INFO("GPU Cull: 可见 {} 个物体（GPU 计数缓冲读回，第 {} 次）", count, s_ReadbackTimes);
        }
    }

    if (count == 0) { m_LastVisibleCount = 0; return; }
    if (count > kMaxObjects) count = kMaxObjects;

    outVisibleIndices.resize(count);
    auto* icB = ResB(m_IndirectCmdBuf);
    HE_ASSERT(icB, "GPUCulling: 间接命令缓冲句柄解析失败");
    void* vi  = icB ? icB->Map() : nullptr;
    if (vi) {
        auto* cmds = static_cast<IndirectDrawCommand*>(vi);
        for (u32 i = 0; i < count; ++i)
            outVisibleIndices[i] = cmds[i].firstInstance;
        // 【T7 诊断】HE_T7_DEBUG=1 时打印前几条 GPU 写出的间接命令（前 3 次读回），
        // 用于核对 indexCount/firstIndex/firstInstance 是否与合批一致（排查画面错乱）。
        if (std::getenv("HE_T7_DEBUG")) {
            static u32 s_DumpTimes = 0;
            if (++s_DumpTimes <= 3u) {
                const u32 n = (count < 8u) ? count : 8u;
                for (u32 i = 0; i < n; ++i) {
                    HE_CORE_INFO("[T7] 间接命令[{}]: indexCount={} instanceCount={} firstIndex={} "
                                 "vertexOffset={} firstInstance={}",
                                 i, cmds[i].indexCount, cmds[i].instanceCount, cmds[i].firstIndex,
                                 cmds[i].vertexOffset, cmds[i].firstInstance);
                }
                // 完整可见集合（最多 64 条）：用于对照"开/关 Hi-Z"两个集合的差集
                std::string ids;
                for (u32 i = 0; i < count && i < 64u; ++i) ids += std::to_string(cmds[i].firstInstance) + " ";
                HE_CORE_INFO("[T7] 可见集合({}): {}", count, ids);
            }
        }
        if (auto* ub = ResB(m_IndirectCmdBuf)) ub->Unmap();
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
        m_PTGParamBuf = device->CreateBufferHandle(paramDesc);   // A-3
        HE_ASSERT(m_PTGParamBuf.IsValid(), "GPUCulling: PTG 参数缓冲句柄创建失败");

        // 清零初始化，确保 frameIndex = 0（PTG 等待帧索引 >= 1）
        auto* ptgB   = ResB(m_PTGParamBuf);
        HE_ASSERT(ptgB, "GPUCulling: PTG 参数缓冲句柄解析失败（已销毁？）");
        void* ptr    = ptgB ? ptgB->Map() : nullptr;
        if (ptr) {
            std::memset(ptr, 0, sizeof(PTGParams));
            if (auto* ptgu = ResB(m_PTGParamBuf)) ptgu->Unmap();
        }
    }

    // 2. 加载 PTG Shader
    m_PTGCS.stage = rhi::ShaderStage::Compute;
    m_PTGCS.spirv = k_PersistentCull_comp_spv;
    m_PTGCS.dxil.assign(std::begin(k_PersistentCull_comp_dxil), std::end(k_PersistentCull_comp_dxil));
    m_PTGCS.dxil.assign(std::begin(k_PersistentCull_comp_dxil), std::end(k_PersistentCull_comp_dxil));
    m_PTGCS.dxil.assign(std::begin(k_PersistentCull_comp_dxil), std::end(k_PersistentCull_comp_dxil));
    m_PTGCS.entryPoint = "main";

    // 3. 创建 PTG 描述符布局（5 个 binding）
    rhi::DescriptorSetLayoutDesc ptgLayout;
    ptgLayout.bindings = {
        {0, rhi::DescriptorType::UniformBuffer, 1, rhi::kStageMaskCompute},        // u_PTGParams
        {1, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},        // u_SceneObjects
        {2, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute}, // u_HiZDepth + Sampler
        {3, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},        // u_IndirectCommands
        {4, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},        // u_DrawCount
    };
    m_PTGLayout = device->CreateDescriptorSetLayout(ptgLayout);
    m_PTGSet = device->AllocateDescriptorSet(m_PTGLayout);

    // 4. 绑定 PTG 参数缓冲（binding 0）
    device->UpdateDescriptorSet(m_PTGSet, 0, rhi::DescriptorType::UniformBuffer, ResB(m_PTGParamBuf));
    // binding 1 (SceneObjects) 由 SetSceneBuffer 设置
    // binding 2 (HiZDepth) 由 SetDepthTexture 设置
    // binding 3 (IndirectCmds) 由 Initialize 中 m_IndirectCmdBuf 设置
    device->UpdateDescriptorSet(m_PTGSet, 3, rhi::DescriptorType::StorageBuffer, ResB(m_IndirectCmdBuf));
    // binding 4 (DrawCount) 由 Initialize 中 m_DrawCountBuf 设置
    device->UpdateDescriptorSet(m_PTGSet, 4, rhi::DescriptorType::StorageBuffer, ResB(m_DrawCountBuf));

    // 5. 创建 PTG PSO（无 push constants，全部通过描述符传递）
    rhi::PipelineStateDesc ptgDesc;
    ptgDesc.bindPoint = rhi::PipelineBindPoint::Compute;
    ptgDesc.computeShader = &m_PTGCS;
    ptgDesc.pushConstantRanges = {};
    ptgDesc.descriptorSetLayouts = {m_PTGLayout};
    ptgDesc.debugName = "PersistentCull";
    m_PTG_PSO = device->CreatePipelineStateHandle(ptgDesc);
    HE_ASSERT(m_PTG_PSO.IsValid(), "GPUCulling: PersistentCull PSO creation failed");

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
    he::Frustum::ToPlaneArray(he::Frustum::FromViewProj(viewProj), planes);

    // 写入 PTGParams 到 Uniform Buffer
    auto* ptgB   = ResB(m_PTGParamBuf);
    HE_ASSERT(ptgB, "GPUCulling: PTG 参数缓冲句柄解析失败（已销毁？）");
    void* mapped = ptgB ? ptgB->Map() : nullptr;
    if (!mapped) return;

    auto* params = static_cast<PTGParams*>(mapped);
    params->viewProj = viewProj;
    for (int i = 0; i < 6; ++i) params->frustumPlanes[i] = planes[i];
    params->screenSize = float2((float)screenW, (float)screenH);
    params->objectCount = objectCount;
    params->hizMipCount = (m_FrameIndex == 0) ? 0 : m_HiZMipCount;
    m_FrameIndex++;
    params->frameIndex = m_PTGFrameCounter++;

    if (auto* ptgu = ResB(m_PTGParamBuf)) ptgu->Unmap();

    // 每帧 Dispatch PTG shader（替代持久化 spin-wait）
    cmd->SetPipeline(ResP(m_PTG_PSO));
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_PTGSet);
    // Dispatch 调试 marker（RenderDoc 定位用）
    cmd->SetDrawDebugLabel("Cull PTG");
    cmd->Dispatch(kPTGGroupCount, 1, 1);

    // 全局屏障：确保 Compute Shader 写入对后续 Indirect Draw 可见
    cmd->PipelineBarrier(
        rhi::PipelineStage::ComputeShader, rhi::PipelineStage::DrawIndirect,
        rhi::ResourceState::UnorderedAccess, rhi::ResourceState::IndirectArgument);
    // 【T7】PTG 已把间接命令与计数写进 GPU 缓冲，绘制端用 `DrawIndexedIndirectCount` 直接消费，
    // 因此这里**不再**清零 m_LastVisibleCount（它只表示上一帧 Readback 的统计值）。
    // 计数缓冲的清零由着色器在派发开头完成（见 PersistentCull.comp.slang 的 u_DrawCount[0] = 0）。
}

// ============================================================
// ShutdownPTG — 发送退出信号并等待 PTG 线程终止
// ============================================================

void GPUCulling::ShutdownPTG(rhi::IRHIDevice* device) {
    if (!m_PTGActive || !device) return;

    // 1. 发送退出信号（frameIndex = 0xFFFFFFFF）
    auto* ptgB   = ResB(m_PTGParamBuf);
    HE_ASSERT(ptgB, "GPUCulling: PTG 参数缓冲句柄解析失败（已销毁？）");
    void* mapped = ptgB ? ptgB->Map() : nullptr;
    if (mapped) {
        auto* params = static_cast<PTGParams*>(mapped);
        params->frameIndex = 0xFFFFFFFF;
        if (auto* ptgu = ResB(m_PTGParamBuf)) ptgu->Unmap();
    }

    // 2. 等待 GPU 完成，确保 PTG 线程检测到退出信号并终止
    device->WaitIdle();

    m_PTGActive = false;
    m_PTGFrameCounter = 0;
    HE_CORE_INFO("GPUCulling: PTG shutdown complete");
}

} // namespace he::render
