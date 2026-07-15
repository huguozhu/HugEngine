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
    // Billboard 顶点 (6 vertices × float4)
    float4 verts[6] = {
        float4(-1,-1, 0,0), float4( 1,-1, 1,0), float4(-1, 1, 0,1),
        float4(-1, 1, 0,1), float4( 1,-1, 1,0), float4( 1, 1, 1,1),
    };
    {
        rhi::BufferDesc d; d.size = sizeof(verts); d.usage = rhi::BufferUsage::Storage;
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
    // Counters
    {
        rhi::BufferDesc d; d.size = sizeof(ParticleCounters); d.usage = rhi::BufferUsage::Storage;
        counters = device->CreateBuffer(d);
    }
    // Random floats
    {
        rhi::BufferDesc d; d.size = sizeof(float) * kRandomFloatNum; d.usage = rhi::BufferUsage::Storage;
        randomFloats = device->CreateBuffer(d);
        float* mapped = static_cast<float*>(randomFloats->Map());
        for (u32 i = 0; i < kRandomFloatNum; ++i)
            mapped[i] = (float)(rand() % 10000) / 10000.0f;
        randomFloats->Unmap();
    }
    // Uniform buffers
    {
        rhi::BufferDesc d; d.size = sizeof(GpuEmitParam); d.usage = rhi::BufferUsage::Uniform;
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
            {0, rhi::DescriptorType::StorageBuffer, 1, 16},         // Billboard vertices
            {1, rhi::DescriptorType::StorageBuffer, 1, 16},         // SortIndices
            {2, rhi::DescriptorType::StorageBuffer, 1, 16},         // Particle buffer
            {7, rhi::DescriptorType::CombinedImageSampler, 1, 16},  // ColorOverLife
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
        desc.colorFormats[0] = rhi::Format::RGBA16_FLOAT;  // HDR target

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
    m_InitPSO.reset(); m_EmitPSO.reset(); m_SimPSO.reset(); m_CullingPSO.reset(); m_RenderPSO.reset();
    if (m_InitLayout    != rhi::kInvalidLayout) device->DestroyDescriptorSetLayout(m_InitLayout);
    if (m_EmitLayout    != rhi::kInvalidLayout) device->DestroyDescriptorSetLayout(m_EmitLayout);
    if (m_SimLayout     != rhi::kInvalidLayout) device->DestroyDescriptorSetLayout(m_SimLayout);
    if (m_CullingLayout != rhi::kInvalidLayout) device->DestroyDescriptorSetLayout(m_CullingLayout);
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

    // Render descriptor set
    cs.renderSet = device->AllocateDescriptorSet(m_RenderLayout);
    device->UpdateDescriptorSet(cs.renderSet, 0, rhi::DescriptorType::StorageBuffer, cs.billboardVB.get());
    device->UpdateDescriptorSet(cs.renderSet, 1, rhi::DescriptorType::StorageBuffer, cs.sortIndices.get());
    device->UpdateDescriptorSet(cs.renderSet, 2, rhi::DescriptorType::StorageBuffer, cs.particleBuf.get());
    device->UpdateDescriptorSet(cs.renderSet, 7, rhi::DescriptorType::CombinedImageSampler,
                                cs.colorOverLifeTex.get(), cs.gradientSampler.get());

    u32 id = (u32)m_Components.size();
    m_Components.push_back(std::move(cs));
    comp->SetGPUReady(true);

    HE_CORE_INFO("ParticleRenderer: 注册组件 id={} maxParticles={}", id, cs.maxParticles);
    return id;
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

    // ── 计算本帧发射数 ──
    cs.elapsed += deltaTime;
    float minInterval = 1.0f / comp->GetParam().particlesPerSec;
    u32 emitCount = 0;
    if (cs.elapsed - cs.lastEmitTime > minInterval) {
        emitCount = u32((cs.elapsed - cs.lastEmitTime) * comp->GetParam().particlesPerSec);
        if (emitCount > cs.maxParticles) emitCount = cs.maxParticles;
        cs.lastEmitTime += emitCount * minInterval;
    }

    // ── 首次: 初始化粒子池 ──
    if (comp->GetElapsedTime() < deltaTime * 1.1f) {
        u32 maxP = cs.maxParticles;
        // 清零 counters
        ParticleCounters zeros = {};
        std::memcpy(cs.counters->Map(), &zeros, sizeof(ParticleCounters));
        cs.counters->Unmap();

        cmd->SetPipeline(m_InitPSO.get());
        cmd->BindDescriptorSet(0, cs.initSet);
        cmd->SetPushConstants(0, sizeof(u32), &maxP);
        cmd->Dispatch((cs.maxParticles + kCS_Threads - 1) / kCS_Threads, 1, 1);

        // Barrier: Init UAV → Emit SRV/UAV
        cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                             rhi::ResourceState::UnorderedAccess, rhi::ResourceState::UnorderedAccess);
    }

    // ── Emit Pass ──
    if (emitCount > 0) {
        // 更新 Emit 参数
        GpuEmitParam emitParam = {};
        emitParam.position       = comp->GetWorldEmitPosition();
        emitParam.maxParticles   = cs.maxParticles;
        emitParam.minInitSpeed   = comp->GetParam().minInitSpeed;
        emitParam.maxInitSpeed   = comp->GetParam().maxInitSpeed;
        emitParam.minLifeTime    = comp->GetParam().minLifeTime;
        emitParam.maxLifeTime    = comp->GetParam().maxLifeTime;
        emitParam.emitShape      = (i32)comp->GetParam().emitShape;
        emitParam.sphereRadius   = comp->GetParam().sphereRadius;
        emitParam.boxSize        = comp->GetParam().boxSize;
        emitParam.direction      = comp->GetParam().direction;
        emitParam.directionSpreadPercent = comp->GetParam().directionSpread;
        emitParam.emitDirectionType = (u32)comp->GetParam().emitDirectionType;
        emitParam.texRowsCols    = comp->GetParam().texRowsCols;
        emitParam.texTimeSampling = (u32)comp->GetParam().texTimeSampling;
        std::memcpy(cs.emitUB->Map(), &emitParam, sizeof(GpuEmitParam));
        cs.emitUB->Unmap();

        // 写 emit_count 到 counters (用于 Emit shader 读取)
        ParticleCounters ctrs;
        std::memcpy(&ctrs, cs.counters->Map(), sizeof(ParticleCounters));
        ctrs.emitCount = emitCount;
        ctrs.aliveCount[0] = 0;  // 重置 alive_pre
        std::memcpy(cs.counters->Map(), &ctrs, sizeof(ParticleCounters));
        cs.counters->Unmap();

        cmd->SetPipeline(m_EmitPSO.get());
        cmd->BindDescriptorSet(0, cs.emitSet);
        cmd->SetPushConstants(0, sizeof(GpuEmitParam), &emitParam);
        cmd->Dispatch((emitCount + kCS_Threads - 1) / kCS_Threads, 1, 1);

        cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                             rhi::ResourceState::UnorderedAccess, rhi::ResourceState::UnorderedAccess);
    }

    // ── Simulate Pass ──
    {
        GpuSimulateParam simParam = {};
        simParam.deltaTime    = deltaTime;
        simParam.gravity      = comp->GetParam().gravity;
        simParam.texRowsCols  = comp->GetParam().texRowsCols;
        simParam.texFramesPerSec = comp->GetParam().texFramesPerSec;
        simParam.texTimeSampling = (u32)comp->GetParam().texTimeSampling;

        // 更新 alive_post_count = 0
        // (Emit 已设置 alive_pre_count, Sim 读 alive_pre 写 alive_post)

        cmd->SetPipeline(m_SimPSO.get());
        cmd->BindDescriptorSet(0, cs.simSet);
        cmd->SetPushConstants(0, sizeof(GpuSimulateParam), &simParam);
        cmd->Dispatch((cs.maxParticles + kCS_Threads - 1) / kCS_Threads, 1, 1);

        cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                             rhi::ResourceState::UnorderedAccess, rhi::ResourceState::UnorderedAccess);
    }

    // ── Culling Pass ──
    {
        GpuCullingParam cullParam = {};
        cullParam.viewProj = viewProj;
        ExtractFrustumPlanes(viewProj, cullParam.frustumPlanes);

        // upload to UB
        std::memcpy(cs.cullingUB->Map(), &cullParam, sizeof(GpuCullingParam));
        cs.cullingUB->Unmap();

        cmd->SetPipeline(m_CullingPSO.get());
        cmd->BindDescriptorSet(0, cs.cullingSet);
        cmd->SetPushConstants(0, sizeof(GpuCullingParam), &cullParam);
        cmd->Dispatch((cs.maxParticles + kCS_Threads - 1) / kCS_Threads, 1, 1);
    }
}

// ============================================================
// Render — 粒子 Billboard 渲染
// ============================================================

void ParticleRenderer::Render(rhi::IRHICommandList* cmd, u32 id,
                               const float4x4& viewProj, const CameraData& camera) {
    if (id >= m_Components.size() || !m_Initialized) return;
    auto& cs = m_Components[id];
    if (!cs.buffersCreated) return;

    // 上传渲染参数
    GpuRenderParam renderParam = {};
    renderParam.viewMatrix  = camera.GetViewMatrix();
    renderParam.projMatrix  = camera.GetProjMatrix();
    renderParam.texRowsCols = cs.comp ? cs.comp->GetParam().texRowsCols : uint2(1,1);
    std::memcpy(cs.renderUB->Map(), &renderParam, sizeof(GpuRenderParam));
    cs.renderUB->Unmap();

    // 绑定管线 + 描述符
    cmd->SetPipeline(m_RenderPSO.get());
    cmd->BindDescriptorSet(0, cs.renderSet);
    cmd->SetPushConstants(0, sizeof(GpuRenderParam), &renderParam);

    // 读取粒子渲染数量（从 GPU counters 回读）
    ParticleCounters ctrs;
    std::memcpy(&ctrs, cs.counters->Map(), sizeof(ParticleCounters));
    cs.counters->Unmap();

    u32 renderCount = ctrs.renderCount;
    if (renderCount > 0) {
        // 6 顶点 Billboard × N 实例
        cmd->Draw(6, renderCount, 0, 0);
    }
}

rhi::IRHIBuffer* ParticleRenderer::GetDrawIndirectBuffer(u32 id) const {
    if (id >= m_Components.size()) return nullptr;
    return m_Components[id].drawIndirectArgs.get();
}

} // namespace he::render
