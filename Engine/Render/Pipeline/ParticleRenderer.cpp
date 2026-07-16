// ParticleRenderer.cpp — GPU 粒子系统完整实现

#include "Pipeline/ParticleRenderer.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include <cstring>

// Slang → SPIR-V headers
#include "ParticleInit.comp.spv.h"
#include "ParticleEmit.comp.spv.h"
#include "ParticleSimulate.comp.spv.h"
#include "ParticleCulling.comp.spv.h"
#include "ParticleSort.comp.spv.h"
#include "ParticleRender.vert.spv.h"
#include "ParticleRender.frag.spv.h"

namespace he::render {

using he::Particle;
using he::ParticleCounters;
using he::SortInfo;
using he::GpuEmitParam;
using he::GpuSimulateParam;
using he::GpuCullingParam;
using he::GpuRenderParam;
using he::ParticleDrawArgs;
using he::DispatchArgs;

static constexpr u32 kRandomFloatNum = 512;
static constexpr u32 kCS_Threads    = 32;

// 从 ViewProj 矩阵提取 6 个视锥平面 (NDC space)
static void ExtractFrustumPlanes(const float4x4& vp, float4 planes[6]) {
    // Left:   row3 + row0
    planes[0] = float4(vp[0][3] + vp[0][0], vp[1][3] + vp[1][0], vp[2][3] + vp[2][0], vp[3][3] + vp[3][0]);
    // Right:  row3 - row0
    planes[1] = float4(vp[0][3] - vp[0][0], vp[1][3] - vp[1][0], vp[2][3] - vp[2][0], vp[3][3] - vp[3][0]);
    // Bottom: row3 + row1
    planes[2] = float4(vp[0][3] + vp[0][1], vp[1][3] + vp[1][1], vp[2][3] + vp[2][1], vp[3][3] + vp[3][1]);
    // Top:    row3 - row1
    planes[3] = float4(vp[0][3] - vp[0][1], vp[1][3] - vp[1][1], vp[2][3] - vp[2][1], vp[3][3] - vp[3][1]);
    // Near:   row2
    planes[4] = float4(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
    // Far:    row3 - row2
    planes[5] = float4(vp[0][3] - vp[0][2], vp[1][3] - vp[1][2], vp[2][3] - vp[2][2], vp[3][3] - vp[3][2]);
    // Normalize all planes
    for (int i = 0; i < 6; i++) {
        float len = sqrt(planes[i].x * planes[i].x + planes[i].y * planes[i].y + planes[i].z * planes[i].z);
        planes[i] /= len;
    }
}

// ============================================================
// CompState::CreateBuffers
// ============================================================

void ParticleRenderer::CompState::CreateBuffers(rhi::IRHIDevice* device, u32 sortCapacity) {
    // Billboard 顶点 (6 vertices × float4) — CPU 写入一次
    float4 verts[6] = {
        float4(-1,-1, 0,0), float4( 1,-1, 1,0), float4(-1, 1, 0,1),
        float4(-1, 1, 0,1), float4( 1,-1, 1,0), float4( 1, 1, 1,1),
    };
    {
        rhi::BufferDesc d; d.size = sizeof(verts); d.usage = rhi::BufferUsage::Storage; d.cpuAccess = true;
        billboardVB = device->CreateBuffer(d);
        std::memcpy(billboardVB->Map(), verts, sizeof(verts));
        billboardVB->Unmap();
    }
    // Particle pool
    {
        rhi::BufferDesc d; d.size = sizeof(Particle) * maxParticles;
        d.usage = rhi::BufferUsage::Storage;
        particleBuf = device->CreateBuffer(d);
    }
    // DeadList + AliveIndices
    {
        rhi::BufferDesc d; d.size = sizeof(u32) * maxParticles; d.usage = rhi::BufferUsage::Storage;
        deadList  = device->CreateBuffer(d);
        alivePre  = device->CreateBuffer(d);
        alivePost = device->CreateBuffer(d);
    }
    // SortIndices
    {
        rhi::BufferDesc d; d.size = sizeof(SortInfo) * sortCapacity; d.usage = rhi::BufferUsage::Storage;
        sortIndices = device->CreateBuffer(d);
    }
    // Counters — CPU 读回渲染数量，需要 host-visible，初始化为零避免垃圾数据
    {
        rhi::BufferDesc d; d.size = sizeof(ParticleCounters); d.usage = rhi::BufferUsage::Storage; d.cpuAccess = true;
        counters = device->CreateBuffer(d);
        ParticleCounters zero = {};
        std::memcpy(counters->Map(), &zero, sizeof(ParticleCounters));
        counters->Unmap();
    }
    // Random floats — CPU 写入一次
    {
        rhi::BufferDesc d; d.size = sizeof(float) * kRandomFloatNum; d.usage = rhi::BufferUsage::Storage; d.cpuAccess = true;
        randomFloats = device->CreateBuffer(d);
        float* mapped = static_cast<float*>(randomFloats->Map());
        for (u32 i = 0; i < kRandomFloatNum; ++i)
            mapped[i] = (float)(rand() % 10000) / 10000.0f;
        randomFloats->Unmap();
    }
    // Uniform buffers — CPU 写入每帧参数，需要 host-visible
    {
        rhi::BufferDesc d; d.size = sizeof(GpuEmitParam); d.usage = rhi::BufferUsage::Uniform; d.cpuAccess = true;
        emitUB    = device->CreateBuffer(d);
        d.size = sizeof(GpuSimulateParam);
        simUB     = device->CreateBuffer(d);
        d.size = sizeof(GpuCullingParam);
        cullingUB = device->CreateBuffer(d);
        d.size = sizeof(GpuRenderParam);
        renderUB  = device->CreateBuffer(d);
    }
    // Indirect args
    {
        rhi::BufferDesc d; d.size = sizeof(ParticleDrawArgs);
        d.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect;
        drawIndirectArgs = device->CreateBuffer(d);
        d.size = sizeof(DispatchArgs);
        emitIndirectArgs = device->CreateBuffer(d);
        simIndirectArgs  = device->CreateBuffer(d);
    }

    // 渐变纹理 (32×1, ColorOverLife RGBA8 + SizeOverLife RG32F)
    {
        rhi::TextureDesc td;
        td.format = rhi::Format::RGBA8_UNORM;
        td.width = 32; td.height = 1; td.depth = 1;
        td.usage = rhi::TextureUsage::ShaderResource;
        colorOverLifeTex = device->CreateTexture(td);

        rhi::SamplerDesc sd;
        sd.minFilter = rhi::FilterMode::Linear;
        sd.magFilter = rhi::FilterMode::Linear;
        sd.addressU = rhi::AddressMode::ClampToEdge;
        gradientSampler = device->CreateSampler(sd);
    }

    buffersCreated = true;
}

void ParticleRenderer::CompState::UpdateGradientTextures(rhi::IRHIDevice* device) {
    if (!comp) return;

    // 生成 ColorOverLife 渐变 (32 samples)
    u8 colorData[32 * 4];  // 32 pixels × RGBA8
    const auto& colorCurve = comp->GetParam().colorOverLife;
    float delta = 1.0f / 31.0f;
    for (u32 i = 0; i < 32; ++i) {
        float t = i * delta;
        float4 c = float4(1.0f);  // default white
        if (colorCurve.size() >= 2) {
            // 线性插值关键帧
            for (size_t k = 0; k < colorCurve.size() - 1; ++k) {
                if (t >= colorCurve[k].first && t <= colorCurve[k+1].first) {
                    float range = colorCurve[k+1].first - colorCurve[k].first;
                    float factor = (range > 0) ? (t - colorCurve[k].first) / range : 0;
                    c = colorCurve[k].second + (colorCurve[k+1].second - colorCurve[k].second) * factor;
                    break;
                }
            }
        }
        colorData[i*4+0] = u8(c.x * 255);
        colorData[i*4+1] = u8(c.y * 255);
        colorData[i*4+2] = u8(c.z * 255);
        colorData[i*4+3] = u8(c.w * 255);
    }

    // 上传到纹理 (简化: 用 Buffer 中转)
    // TODO: 通过 RHI 正确上传纹理数据
    (void)colorData; (void)device;
}

// ============================================================
// Initialize
// ============================================================

bool ParticleRenderer::Initialize(rhi::IRHIDevice* device) {
    m_Device = device;

    // Load shader bytecodes
    m_InitCS.stage      = rhi::ShaderStage::Compute;
    m_InitCS.spirv      = k_ParticleInit_comp_spv;
    m_InitCS.entryPoint = "main";

    m_EmitCS.stage      = rhi::ShaderStage::Compute;
    m_EmitCS.spirv      = k_ParticleEmit_comp_spv;
    m_EmitCS.entryPoint = "main";

    m_SimCS.stage       = rhi::ShaderStage::Compute;
    m_SimCS.spirv       = k_ParticleSimulate_comp_spv;
    m_SimCS.entryPoint  = "main";

    m_CullingCS.stage      = rhi::ShaderStage::Compute;
    m_CullingCS.spirv      = k_ParticleCulling_comp_spv;
    m_CullingCS.entryPoint = "main";

    m_SortCS.stage      = rhi::ShaderStage::Compute;
    m_SortCS.spirv      = k_ParticleSort_comp_spv;
    m_SortCS.entryPoint = "main";

    // Layouts
    auto createLayout = [device](std::vector<rhi::DescriptorSetLayoutBinding> b) {
        rhi::DescriptorSetLayoutDesc ld; ld.bindings = std::move(b);
        return device->CreateDescriptorSetLayout(ld);
    };

    rhi::PushConstantRange pcRange;
    pcRange.stageMask = 32; // VK_SHADER_STAGE_COMPUTE_BIT
    pcRange.offset = 0; pcRange.size = 256; // Vulkan 保证最小值 128B，部分 GPU 支持 256B

    auto createComputePSO = [&](rhi::DescriptorSetLayoutHandle layout,
                                 rhi::ShaderBytecode* cs, const char* name) {
        rhi::PipelineStateDesc desc;
        desc.bindPoint         = rhi::PipelineBindPoint::Compute;
        desc.computeShader     = cs;
        desc.pushConstantRanges = {pcRange};
        desc.descriptorSetLayouts = {layout};
        desc.debugName         = name;
        return device->CreatePipelineState(desc);
    };

    // Init Layout + PSO
    m_InitLayout = createLayout({{0, rhi::DescriptorType::StorageBuffer, 1, 32},
                                  {1, rhi::DescriptorType::StorageBuffer, 1, 32}});
    m_InitPSO = createComputePSO(m_InitLayout, &m_InitCS, "ParticleInit");

    // Emit Layout + PSO
    m_EmitLayout = createLayout({{0, rhi::DescriptorType::StorageBuffer, 1, 32},
                                  {1, rhi::DescriptorType::StorageBuffer, 1, 32},
                                  {2, rhi::DescriptorType::StorageBuffer, 1, 32},
                                  {3, rhi::DescriptorType::StorageBuffer, 1, 32},
                                  {4, rhi::DescriptorType::StorageBuffer, 1, 32}});
    m_EmitPSO = createComputePSO(m_EmitLayout, &m_EmitCS, "ParticleEmit");

    // Sim Layout + PSO
    m_SimLayout = createLayout({{0, rhi::DescriptorType::StorageBuffer, 1, 32},
                                 {1, rhi::DescriptorType::StorageBuffer, 1, 32},
                                 {2, rhi::DescriptorType::StorageBuffer, 1, 32},
                                 {3, rhi::DescriptorType::StorageBuffer, 1, 32},
                                 {4, rhi::DescriptorType::StorageBuffer, 1, 32},
                                 {5, rhi::DescriptorType::StorageBuffer, 1, 32}});
    m_SimPSO = createComputePSO(m_SimLayout, &m_SimCS, "ParticleSimulate");

    // Culling Layout + PSO
    m_CullingLayout = createLayout({{0, rhi::DescriptorType::StorageBuffer, 1, 32},
                                     {1, rhi::DescriptorType::StorageBuffer, 1, 32},
                                     {2, rhi::DescriptorType::StorageBuffer, 1, 32},
                                     {3, rhi::DescriptorType::StorageBuffer, 1, 32},
                                     {4, rhi::DescriptorType::StorageBuffer, 1, 32}});
    m_CullingPSO = createComputePSO(m_CullingLayout, &m_CullingCS, "ParticleCulling");

    // Sort Layout + PSO
    m_SortLayout = createLayout({{0, rhi::DescriptorType::StorageBuffer, 1, 32},
                                  {1, rhi::DescriptorType::StorageBuffer, 1, 32}});
    m_SortPSO = createComputePSO(m_SortLayout, &m_SortCS, "ParticleSort");

    // ── Render PSO (Graphics: Billboard) ──
    m_RenderVS.stage      = rhi::ShaderStage::Vertex;
    m_RenderVS.spirv      = k_ParticleRender_vert_spv;
    m_RenderVS.entryPoint = "vertexMain";

    m_RenderFS.stage      = rhi::ShaderStage::Pixel;
    m_RenderFS.spirv      = k_ParticleRender_frag_spv;
    m_RenderFS.entryPoint = "fragmentMain";

    {
        rhi::DescriptorSetLayoutDesc ld;
        ld.bindings = {
            {0, rhi::DescriptorType::StorageBuffer, 1, 1},          // Billboard vertices (Vertex)
            {1, rhi::DescriptorType::StorageBuffer, 1, 1},          // SortIndices (Vertex)
            {2, rhi::DescriptorType::StorageBuffer, 1, 1},          // Particle buffer (Vertex)
            {3, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // SceneDepth (Fragment, 软粒子)
        };
        m_RenderLayout = device->CreateDescriptorSetLayout(ld);

        rhi::PipelineStateDesc desc;
        desc.bindPoint   = rhi::PipelineBindPoint::Graphics;
        desc.vertexShader = &m_RenderVS;
        desc.pixelShader  = &m_RenderFS;
        desc.pushConstantRanges = {pcRange};
        desc.descriptorSetLayouts = {m_RenderLayout};
        desc.debugName   = "ParticleRender";
        desc.depthTest    = true;
        desc.depthWrite   = false;  // 粒子不写深度
        desc.depthFormat  = rhi::Format::D32_FLOAT;    // 匹配 HDR 深度附件
        desc.colorFormats[0] = rhi::Format::RGBA16_FLOAT;  // HDR target
        desc.colorLoadOp  = rhi::LoadOp::Load;  // 保留 HDR Target 已有内容（Lighting 结果）
        desc.depthLoadOp  = rhi::LoadOp::Load;  // 保留深度缓冲已有内容

        // Push constant for render (Vertex + Fragment stages)
        rhi::PushConstantRange renderPCRange;
        renderPCRange.stageMask = 1 | 16;  // VERTEX_BIT | FRAGMENT_BIT
        renderPCRange.offset = 0; renderPCRange.size = 256;
        desc.pushConstantRanges = {renderPCRange};

        m_RenderPSO = device->CreatePipelineState(desc);
    }

    m_Initialized = true;
    HE_CORE_INFO("ParticleRenderer: 初始化完成");
    return true;
}

void ParticleRenderer::Shutdown(rhi::IRHIDevice* device) {
    m_Components.clear();
    m_InitPSO.reset(); m_EmitPSO.reset(); m_SimPSO.reset(); m_CullingPSO.reset(); m_SortPSO.reset(); m_RenderPSO.reset();
    if (m_InitLayout    != rhi::kInvalidLayout) device->DestroyDescriptorSetLayout(m_InitLayout);
    if (m_EmitLayout    != rhi::kInvalidLayout) device->DestroyDescriptorSetLayout(m_EmitLayout);
    if (m_SimLayout     != rhi::kInvalidLayout) device->DestroyDescriptorSetLayout(m_SimLayout);
    if (m_CullingLayout != rhi::kInvalidLayout) device->DestroyDescriptorSetLayout(m_CullingLayout);
    if (m_SortLayout    != rhi::kInvalidLayout) device->DestroyDescriptorSetLayout(m_SortLayout);
    m_Initialized = false;
}

// ============================================================
// RegisterComponent
// ============================================================

u32 ParticleRenderer::RegisterComponent(ParticleComponent* comp, rhi::IRHIDevice* device) {
    CompState cs;
    cs.comp         = comp;
    cs.maxParticles = comp->GetMaxParticles();
    u32 sortCap = 1; while (sortCap < cs.maxParticles) sortCap *= 2;
    cs.CreateBuffers(device, sortCap);

    // 创建 DescriptorSets 并绑定 buffers
    cs.initSet = device->AllocateDescriptorSet(m_InitLayout);
    device->UpdateDescriptorSet(cs.initSet, 0, rhi::DescriptorType::StorageBuffer, cs.deadList.get());
    device->UpdateDescriptorSet(cs.initSet, 1, rhi::DescriptorType::StorageBuffer, cs.counters.get());

    cs.emitSet = device->AllocateDescriptorSet(m_EmitLayout);
    device->UpdateDescriptorSet(cs.emitSet, 0, rhi::DescriptorType::StorageBuffer, cs.deadList.get());
    device->UpdateDescriptorSet(cs.emitSet, 1, rhi::DescriptorType::StorageBuffer, cs.counters.get());
    device->UpdateDescriptorSet(cs.emitSet, 2, rhi::DescriptorType::StorageBuffer, cs.alivePre.get());
    device->UpdateDescriptorSet(cs.emitSet, 3, rhi::DescriptorType::StorageBuffer, cs.particleBuf.get());
    device->UpdateDescriptorSet(cs.emitSet, 4, rhi::DescriptorType::StorageBuffer, cs.randomFloats.get());

    cs.simSet = device->AllocateDescriptorSet(m_SimLayout);
    device->UpdateDescriptorSet(cs.simSet, 0, rhi::DescriptorType::StorageBuffer, cs.alivePre.get());
    device->UpdateDescriptorSet(cs.simSet, 1, rhi::DescriptorType::StorageBuffer, cs.alivePost.get());
    device->UpdateDescriptorSet(cs.simSet, 2, rhi::DescriptorType::StorageBuffer, cs.counters.get());
    device->UpdateDescriptorSet(cs.simSet, 3, rhi::DescriptorType::StorageBuffer, cs.particleBuf.get());
    device->UpdateDescriptorSet(cs.simSet, 4, rhi::DescriptorType::StorageBuffer, cs.deadList.get());
    device->UpdateDescriptorSet(cs.simSet, 5, rhi::DescriptorType::StorageBuffer, cs.randomFloats.get());

    cs.cullingSet = device->AllocateDescriptorSet(m_CullingLayout);
    device->UpdateDescriptorSet(cs.cullingSet, 0, rhi::DescriptorType::StorageBuffer, cs.alivePost.get());
    device->UpdateDescriptorSet(cs.cullingSet, 1, rhi::DescriptorType::StorageBuffer, cs.particleBuf.get());
    device->UpdateDescriptorSet(cs.cullingSet, 2, rhi::DescriptorType::StorageBuffer, cs.sortIndices.get());
    device->UpdateDescriptorSet(cs.cullingSet, 3, rhi::DescriptorType::StorageBuffer, cs.counters.get());
    device->UpdateDescriptorSet(cs.cullingSet, 4, rhi::DescriptorType::StorageBuffer, cs.drawIndirectArgs.get());

    // Sort descriptor set
    cs.sortSet = device->AllocateDescriptorSet(m_SortLayout);
    device->UpdateDescriptorSet(cs.sortSet, 0, rhi::DescriptorType::StorageBuffer, cs.sortIndices.get());
    device->UpdateDescriptorSet(cs.sortSet, 1, rhi::DescriptorType::StorageBuffer, cs.counters.get());

    // Render descriptor set
    cs.renderSet = device->AllocateDescriptorSet(m_RenderLayout);
    device->UpdateDescriptorSet(cs.renderSet, 0, rhi::DescriptorType::StorageBuffer, cs.billboardVB.get());
    device->UpdateDescriptorSet(cs.renderSet, 1, rhi::DescriptorType::StorageBuffer, cs.sortIndices.get());
    device->UpdateDescriptorSet(cs.renderSet, 2, rhi::DescriptorType::StorageBuffer, cs.particleBuf.get());
    // 场景深度纹理（软粒子），由 DeferredPipeline 在 Init 时通过 SetSceneDepth 设置
    cs.sceneDepthTex = m_SceneDepthTex;
    cs.sceneDepthSampler = m_SceneDepthSampler;
    if (cs.sceneDepthTex && cs.sceneDepthSampler) {
        device->UpdateDescriptorSet(cs.renderSet, 3, rhi::DescriptorType::CombinedImageSampler,
                                    cs.sceneDepthTex, cs.sceneDepthSampler);
    }

    u32 id = (u32)m_Components.size();
    m_Components.push_back(std::move(cs));
    comp->SetGPUReady(true);

    HE_CORE_INFO("ParticleRenderer: 注册组件 id={} maxParticles={}", id, cs.maxParticles);
    return id;
}

// ============================================================
// DispatchCompute — GPU 模拟全流程
// ============================================================

// ============================================================
// DebugDumpState — 回读 GPU 缓冲区并打印完整管线状态
// ============================================================

void ParticleRenderer::DebugDumpState(u32 id, const char* step) {
    if (id >= m_Components.size()) return;
    auto& cs = m_Components[id];
    if (!cs.buffersCreated) return;

    // 回读 Counters
    ParticleCounters ctrs;
    void* cMap = cs.counters->Map();
    if (!cMap) { HE_CORE_WARN("[{}] counters->Map() 返回 nullptr!", step); return; }
    std::memcpy(&ctrs, cMap, sizeof(ParticleCounters));
    cs.counters->Unmap();

    // 回读 DeadList 头部 (前 8 个)
    u32 deadHead[8] = {};
    void* dMap = cs.deadList->Map();
    if (dMap) { std::memcpy(deadHead, dMap, sizeof(deadHead)); cs.deadList->Unmap(); }

    // 回读 AlivePre 头部 (前 8 个)
    u32 alivePreH[8] = {};
    void* apMap = cs.alivePre->Map();
    if (apMap) { std::memcpy(alivePreH, apMap, sizeof(alivePreH)); cs.alivePre->Unmap(); }

    // 回读 AlivePost 头部 (前 8 个)
    u32 alivePostH[8] = {};
    void* apoMap = cs.alivePost->Map();
    if (apoMap) { std::memcpy(alivePostH, apoMap, sizeof(alivePostH)); cs.alivePost->Unmap(); }

    // 回读 SortIndices 头部 (前 8 条)
    SortInfo sorts[8] = {};
    void* sMap = cs.sortIndices->Map();
    if (sMap) { std::memcpy(sorts, sMap, sizeof(sorts)); cs.sortIndices->Unmap(); }

    // 回读前 3 个粒子
    Particle parts[3] = {};
    void* pMap = cs.particleBuf->Map();
    if (pMap) { std::memcpy(parts, pMap, sizeof(parts)); cs.particleBuf->Unmap(); }

    // 回读 DrawIndirectArgs
    ParticleDrawArgs drawArgs = {};
    void* iaMap = cs.drawIndirectArgs->Map();
    if (iaMap) { std::memcpy(&drawArgs, iaMap, sizeof(drawArgs)); cs.drawIndirectArgs->Unmap(); }

    // 回读 EmitIndirectArgs
    DispatchArgs emitIA = {};
    void* eiaMap = cs.emitIndirectArgs->Map();
    if (eiaMap) { std::memcpy(&emitIA, eiaMap, sizeof(emitIA)); cs.emitIndirectArgs->Unmap(); }

    static int dumpSeq = 0;
    HE_CORE_INFO("=== [{}] frame={} step='{}' ===", dumpSeq % 2 == 0 ? "PRE-GPU" : "POST-RECORD", dumpSeq, step);
    HE_CORE_INFO("  Counters: dead={} alivePre={} alivePost={} emit={} sim={} render={}",
        ctrs.deadCount, ctrs.aliveCount[0], ctrs.aliveCount[1],
        ctrs.emitCount, ctrs.simulateCount, ctrs.renderCount);
    HE_CORE_INFO("  DeadList[0..3]: {} {} {} {} ...  AlivePre[0..3]: {} {} {} {} ...",
        deadHead[0], deadHead[1], deadHead[2], deadHead[3],
        alivePreH[0], alivePreH[1], alivePreH[2], alivePreH[3]);
    HE_CORE_INFO("  AlivePost[0..3]: {} {} {} {} ...",
        alivePostH[0], alivePostH[1], alivePostH[2], alivePostH[3]);
    HE_CORE_INFO("  SortIndices[0..3]: (idx={} d={:.3f}) (idx={} d={:.3f}) (idx={} d={:.3f}) (idx={} d={:.3f})",
        sorts[0].particleIndex, sorts[0].particleDepth,
        sorts[1].particleIndex, sorts[1].particleDepth,
        sorts[2].particleIndex, sorts[2].particleDepth,
        sorts[3].particleIndex, sorts[3].particleDepth);
    HE_CORE_INFO("  Particle[0]: life=({:.2f}/{:.2f}/{:.2f}) vel=({:.1f},{:.1f},{:.1f}) pos=({:.1f},{:.1f},{:.1f}) size={:.2f}",
        parts[0].lifeTime.x, parts[0].lifeTime.y, parts[0].lifeTime.z,
        parts[0].velocity.x, parts[0].velocity.y, parts[0].velocity.z,
        parts[0].position.x, parts[0].position.y, parts[0].position.z,
        parts[0].position.w);
    HE_CORE_INFO("  DrawArgs: vtx={} inst={} firstVtx={} firstInst={}",
        drawArgs.vertexCount, drawArgs.instanceCount, drawArgs.firstVertex, drawArgs.firstInstance);
    HE_CORE_INFO("  EmitIndirect: dispatch({},{},{})",
        emitIA.dispatchX, emitIA.dispatchY, emitIA.dispatchZ);
    dumpSeq++;
}

// ============================================================
// DispatchCompute — GPU 模拟全流程
// ============================================================

void ParticleRenderer::DispatchCompute(rhi::IRHICommandList* cmd, u32 id, float deltaTime,
                                        const float4x4& viewProj) {
    if (id >= m_Components.size() || !m_Initialized) return;
    auto& cs = m_Components[id];
    auto* comp = cs.comp;
    if (!comp || comp->GetState() != ParticleState::Playing) return;

    static int dbgFrame = 0;
    bool verbose = (dbgFrame < 5);
    if (verbose) HE_CORE_INFO("--- DispatchCompute frame={} dt={:.4f} ---", dbgFrame, deltaTime);

    // ── 处理 dt 极小或为零（首帧可能 dt≈0，用固定帧率兜底）──
    if (deltaTime < 1e-6f) {
        deltaTime = 1.0f / 60.0f;  // 默认 60fps
        if (verbose) HE_CORE_INFO("  dt≈0 (实际={:.6f}), 使用默认 dt={:.4f}", deltaTime, deltaTime);
    }

    // ── Step 0: 读回上帧 GPU 执行结果（在 CPU 清零 counters 之前缓存 renderCount）──
    DebugDumpState(id, "Dispatch-Start(prev-frame-GPU-result)");
    {
        ParticleCounters prevCtrs;
        void* prevMap = cs.counters->Map();
        if (prevMap) {
            std::memcpy(&prevCtrs, prevMap, sizeof(ParticleCounters));
            cs.cachedRenderCount = prevCtrs.renderCount;  // 缓存上一帧 GPU 输出
            cs.counters->Unmap();
        }
    }

    // ── 首次: 初始化粒子池（仅执行一次，使用 bool flag）──
    if (!cs.initDone) {
        cs.initDone = true;
        HE_CORE_INFO("  [INIT] 初始化 deadList 和 counters (一次性)");
        u32 maxP = cs.maxParticles;
        // 初始化 counters：deadCount = maxParticles（所有槽位初始可用）
        ParticleCounters initCtrs = {};
        initCtrs.deadCount = maxP;
        std::memcpy(cs.counters->Map(), &initCtrs, sizeof(ParticleCounters));
        cs.counters->Unmap();

        if (verbose) DebugDumpState(id, "After-CPU-Init(deadCount-written)");

        cmd->SetPipeline(m_InitPSO.get());
        cmd->BindDescriptorSet(0, cs.initSet);
        cmd->SetPushConstants(0, sizeof(u32), &maxP);
        cmd->Dispatch((cs.maxParticles + kCS_Threads - 1) / kCS_Threads, 1, 1);

        // Barrier: Init UAV → Emit SRV/UAV
        cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                             rhi::ResourceState::UnorderedAccess, rhi::ResourceState::UnorderedAccess);
        if (verbose) HE_CORE_INFO("  [INIT] dispatch 完成 (maxP={}, groups={})",
            maxP, (cs.maxParticles + kCS_Threads - 1) / kCS_Threads);
    }

    // ── 计算本帧发射数 ──
    cs.elapsed += deltaTime;
    float minInterval = 1.0f / comp->GetParam().particlesPerSec;
    u32 emitCount = 0;
    if (cs.elapsed - cs.lastEmitTime > minInterval) {
        emitCount = u32((cs.elapsed - cs.lastEmitTime) * comp->GetParam().particlesPerSec);
        if (emitCount > cs.maxParticles) emitCount = cs.maxParticles;
        cs.lastEmitTime += emitCount * minInterval;
    }
    if (verbose) HE_CORE_INFO("  CPU: elapsed={:.4f} lastEmit={:.4f} emitCount={} maxP={}",
        cs.elapsed, cs.lastEmitTime, emitCount, cs.maxParticles);

    // ── Emit Pass ──
    if (emitCount > 0) {
        if (verbose) HE_CORE_INFO("  [EMIT] emitCount={}", emitCount);

        // 更新 Emit 参数（使用 memcpy 复制 glm 类型到 float 数组，确保 std140 布局匹配）
        GpuEmitParam emitParam = {};
        {
            float3 pos = comp->GetWorldEmitPosition();
            std::memcpy(emitParam.position, &pos, sizeof(float3));
            float3 dir = comp->GetParam().direction;
            std::memcpy(emitParam.direction, &dir, sizeof(float3));
            float3 box = comp->GetParam().boxSize;
            std::memcpy(emitParam.boxSize, &box, sizeof(float3));
            uint2 tc = comp->GetParam().texRowsCols;
            std::memcpy(emitParam.texRowsCols, &tc, sizeof(uint2));
        }
        emitParam.maxParticles   = cs.maxParticles;
        emitParam.minInitSpeed   = comp->GetParam().minInitSpeed;
        emitParam.maxInitSpeed   = comp->GetParam().maxInitSpeed;
        emitParam.minLifeTime    = comp->GetParam().minLifeTime;
        emitParam.maxLifeTime    = comp->GetParam().maxLifeTime;
        emitParam.emitShape      = (i32)comp->GetParam().emitShape;
        emitParam.sphereRadius   = comp->GetParam().sphereRadius;
        emitParam.directionSpread = comp->GetParam().directionSpread;
        emitParam.emitDirectionType = (u32)comp->GetParam().emitDirectionType;
        emitParam.texTimeSampling = (u32)comp->GetParam().texTimeSampling;
        emitParam.minSize = comp->GetParam().minSize;
        emitParam.maxSize = comp->GetParam().maxSize;
        std::memcpy(cs.emitUB->Map(), &emitParam, sizeof(GpuEmitParam));
        cs.emitUB->Unmap();

        if (verbose) HE_CORE_INFO("    emitPos=({:.1f},{:.1f},{:.1f}) shape={} dirType={} radius={:.1f} speed=[{:.1f},{:.1f}]",
            emitParam.position[0], emitParam.position[1], emitParam.position[2],
            emitParam.emitShape, emitParam.emitDirectionType, emitParam.sphereRadius,
            emitParam.minInitSpeed, emitParam.maxInitSpeed);

        // 写 emit_count 到 counters (用于 Emit shader 读取)
        ParticleCounters ctrs;
        void* cMapBefore = cs.counters->Map();
        std::memcpy(&ctrs, cMapBefore, sizeof(ParticleCounters));
        if (verbose) HE_CORE_INFO("    counters-before: dead={} alivePre={} alivePost={} emit={} render={}",
            ctrs.deadCount, ctrs.aliveCount[0], ctrs.aliveCount[1],
            ctrs.emitCount, ctrs.renderCount);
        ctrs.emitCount = emitCount;
        ctrs.aliveCount[0] = 0;  // 重置 alive_pre（本帧新发射粒子）
        ctrs.aliveCount[1] = 0;  // 重置 alive_post（Simulate 每帧重新填充）
        // 注意: simulateCount 不重置 — 用作全局唯一发射计数器（Emit shader 种子生成）
        ctrs.renderCount = 0;
        std::memcpy(cMapBefore, &ctrs, sizeof(ParticleCounters));
        cs.counters->Unmap();

        // 立即回读验证 CPU 写入是否成功（测试 Map/Unmap 对同一 buffer 的读写一致性）
        ParticleCounters verify;
        std::memcpy(&verify, cs.counters->Map(), sizeof(ParticleCounters));
        cs.counters->Unmap();
        if (verbose) HE_CORE_INFO("    counters-after-write-verify: dead={} alivePre={} emit={} render={} (size={})",
            verify.deadCount, verify.aliveCount[0], verify.emitCount, verify.renderCount,
            (u32)sizeof(ParticleCounters));
        HE_CORE_INFO("    GpuEmitParam size={} maxPushConst={}", (u32)sizeof(GpuEmitParam), (u32)sizeof(GpuEmitParam) > 128 ? "OVER!" : "OK");

        cmd->SetPipeline(m_EmitPSO.get());
        cmd->BindDescriptorSet(0, cs.emitSet);
        cmd->SetPushConstants(0, sizeof(GpuEmitParam), &emitParam);
        cmd->Dispatch((emitCount + kCS_Threads - 1) / kCS_Threads, 1, 1);

        cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                             rhi::ResourceState::UnorderedAccess, rhi::ResourceState::UnorderedAccess);
    } else {
        if (verbose) HE_CORE_INFO("  [EMIT] SKIP — emitCount=0");
    }

    // ── Simulate Pass ──
    {
        GpuSimulateParam simParam = {};
        simParam.deltaTime    = deltaTime;
        {
            float3 g = comp->GetParam().gravity;
            std::memcpy(simParam.gravity, &g, sizeof(float3));
            uint2 tc = comp->GetParam().texRowsCols;
            std::memcpy(simParam.texRowsCols, &tc, sizeof(uint2));
        }
        simParam.texFramesPerSec = comp->GetParam().texFramesPerSec;
        simParam.texTimeSampling = (u32)comp->GetParam().texTimeSampling;
        simParam.maxParticles    = cs.maxParticles;  // 粒子池总容量（Simulate 遍历所有槽位）

        cmd->SetPipeline(m_SimPSO.get());
        cmd->BindDescriptorSet(0, cs.simSet);
        cmd->SetPushConstants(0, sizeof(GpuSimulateParam), &simParam);
        cmd->Dispatch((cs.maxParticles + kCS_Threads - 1) / kCS_Threads, 1, 1);

        cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                             rhi::ResourceState::UnorderedAccess, rhi::ResourceState::UnorderedAccess);

        if (verbose) HE_CORE_INFO("  [SIM] dt={:.4f} gravity=({:.1f},{:.1f},{:.1f}) groups={}",
            simParam.deltaTime, simParam.gravity[0], simParam.gravity[1], simParam.gravity[2],
            (cs.maxParticles + kCS_Threads - 1) / kCS_Threads);
    }

    // ── Culling Pass ──
    {
        GpuCullingParam cullParam = {};
        // viewProj: float4x4 → float[4][4]
        std::memcpy(cullParam.viewProj, &viewProj, sizeof(float4x4));
        // frustumPlanes: float4[6] → float[6][4]
        float4 planes[6];
        ExtractFrustumPlanes(viewProj, planes);
        std::memcpy(cullParam.frustumPlanes, planes, sizeof(planes));

        // upload to UB
        std::memcpy(cs.cullingUB->Map(), &cullParam, sizeof(GpuCullingParam));
        cs.cullingUB->Unmap();

        cmd->SetPipeline(m_CullingPSO.get());
        cmd->BindDescriptorSet(0, cs.cullingSet);
        cmd->SetPushConstants(0, sizeof(GpuCullingParam), &cullParam);
        cmd->Dispatch((cs.maxParticles + kCS_Threads - 1) / kCS_Threads, 1, 1);

        if (verbose) {
            HE_CORE_INFO("  [CULL] groups={}", (cs.maxParticles + kCS_Threads - 1) / kCS_Threads);
            HE_CORE_INFO("    Near plane: ({:.2f},{:.2f},{:.2f},{:.2f})",
                cullParam.frustumPlanes[4][0], cullParam.frustumPlanes[4][1],
                cullParam.frustumPlanes[4][2], cullParam.frustumPlanes[4][3]);
        }
    }

    // ── Sort Pass (Bitonic Sort: 按深度降序，远→近渲染) ──
    {
        cmd->SetPipeline(m_SortPSO.get());
        cmd->BindDescriptorSet(0, cs.sortSet);
        cmd->Dispatch(1, 1, 1);  // 单 workgroup 512 线程，shared memory 排序

        cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::VertexShader,
                             rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource);

        if (verbose) HE_CORE_INFO("  [SORT] Bitonic Sort dispatch (512 threads, shared mem)");
    }

    // ── Dispatch 后立即检查 CPU 写入是否正确 ──
    if (verbose) DebugDumpState(id, "Dispatch-End(CPU-writes-recap)");

    dbgFrame++;
}

// ============================================================
// Render — 粒子 Billboard 渲染
// ============================================================

void ParticleRenderer::Render(rhi::IRHICommandList* cmd, u32 id,
                               const float4x4& viewProj, const CameraData& camera) {
    if (id >= m_Components.size() || !m_Initialized) return;
    auto& cs = m_Components[id];
    if (!cs.buffersCreated) return;

    static int renderDbg = 0;
    bool verbose = (renderDbg < 5);

    // 上传渲染参数
    GpuRenderParam renderParam = {};
    {
        float4x4 vm = camera.GetViewMatrix();
        std::memcpy(renderParam.viewMatrix, &vm, sizeof(float4x4));
        float4x4 pm = camera.GetProjMatrix();
        std::memcpy(renderParam.projMatrix, &pm, sizeof(float4x4));
        uint2 tc = cs.comp ? cs.comp->GetParam().texRowsCols : uint2(1,1);
        std::memcpy(renderParam.texRowsCols, &tc, sizeof(uint2));
        float4 sc = cs.comp ? cs.comp->GetParam().startColor : float4(1.0f);
        std::memcpy(renderParam.startColor, &sc, sizeof(float4));
        float4 ec = cs.comp ? cs.comp->GetParam().endColor : float4(1.0f, 0.0f, 0.0f, 0.0f);
        std::memcpy(renderParam.endColor, &ec, sizeof(float4));
    }
    std::memcpy(cs.renderUB->Map(), &renderParam, sizeof(GpuRenderParam));
    cs.renderUB->Unmap();

    // 绑定管线 + 描述符
    cmd->SetPipeline(m_RenderPSO.get());
    cmd->BindDescriptorSet(0, cs.renderSet);
    cmd->SetPushConstants(0, sizeof(GpuRenderParam), &renderParam);

    // 使用 DispatchCompute 中缓存的上一帧 GPU renderCount
    // （DispatchCompute 中 CPU 已清零 renderCount，必须用缓存值避免读到 0）
    u32 renderCount = cs.cachedRenderCount;

    if (verbose) {
        HE_CORE_INFO("=== [RENDER] frame={} renderCount(cached)={} ===",
            renderDbg, renderCount);
        DebugDumpState(id, "Render(read-prev-frame-GPU-output)");
    } else if (renderDbg % 120 == 0) {
        // 定期日志：回读完整 counters 用于诊断
        ParticleCounters ctrs;
        void* dbgMap = cs.counters->Map();
        if (dbgMap) { std::memcpy(&ctrs, dbgMap, sizeof(ParticleCounters)); cs.counters->Unmap(); }
        HE_CORE_INFO("ParticleRender frame={}: emit={} sim={} render(cached)={} dead={}",
            renderDbg, ctrs.emitCount, ctrs.simulateCount,
            renderCount, ctrs.deadCount);
        DebugDumpState(id, "Render(periodic)");
    }

    if (renderCount > 0) {
        HE_CORE_INFO("  >>> DRAW: 6 vertices x {} instances <<<", renderCount);
        // 6 顶点 Billboard × N 实例
        cmd->Draw(6, renderCount, 0, 0);
    } else {
        if (verbose) HE_CORE_INFO("  >>> SKIP DRAW — renderCount=0 <<<");
    }

    renderDbg++;
}

void ParticleRenderer::SetSceneDepth(rhi::IRHITexture* depthTexture, rhi::IRHISampler* depthSampler) {
    m_SceneDepthTex = depthTexture;
    m_SceneDepthSampler = depthSampler;
    // 更新所有已注册组件的深度纹理绑定
    for (auto& cs : m_Components) {
        cs.sceneDepthTex = depthTexture;
        cs.sceneDepthSampler = depthSampler;
        if (cs.renderSet != rhi::kInvalidSet && depthTexture && depthSampler) {
            m_Device->UpdateDescriptorSet(cs.renderSet, 3,
                rhi::DescriptorType::CombinedImageSampler,
                depthTexture, depthSampler);
        }
    }
}

rhi::IRHIBuffer* ParticleRenderer::GetDrawIndirectBuffer(u32 id) const {
    if (id >= m_Components.size()) return nullptr;
    return m_Components[id].drawIndirectArgs.get();
}

} // namespace he::render
