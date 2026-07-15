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
    buffersCreated = true;
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
    pcRange.offset = 0; pcRange.size = 64; // enough for largest push constant

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

    m_Initialized = true;
    HE_CORE_INFO("ParticleRenderer: 初始化完成");
    return true;
}

void ParticleRenderer::Shutdown(rhi::IRHIDevice* device) {
    m_Components.clear();
    m_InitPSO.reset(); m_EmitPSO.reset(); m_SimPSO.reset(); m_CullingPSO.reset();
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

    u32 id = (u32)m_Components.size();
    m_Components.push_back(std::move(cs));
    comp->SetGPUReady(true);

    HE_CORE_INFO("ParticleRenderer: 注册组件 id={} maxParticles={}", id, cs.maxParticles);
    return id;
}

// ============================================================
// DispatchCompute — GPU 模拟全流程
// ============================================================

void ParticleRenderer::DispatchCompute(rhi::IRHICommandList* cmd, u32 id, float deltaTime) {
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
        cmd->SetPushConstants(0, sizeof(GpuEmitParam), cs.emitUB.get() ? nullptr : nullptr);
        // 更新 uniform buffer (参数通过 UB 传递，不在 push constant)
        m_Device->UpdateDescriptorSet(cs.emitSet, 4, rhi::DescriptorType::StorageBuffer, cs.emitUB.get());
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
        // frustum planes 由调用者 (DeferredPipeline) 提供
        // 此处简化: 全部通过

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
    (void)cmd; (void)id; (void)viewProj; (void)camera;
    // TODO: 从 Indirect Buffer 读取 DrawArgs → DrawIndexedIndirect
}

rhi::IRHIBuffer* ParticleRenderer::GetDrawIndirectBuffer(u32 id) const {
    if (id >= m_Components.size()) return nullptr;
    return m_Components[id].drawIndirectArgs.get();
}

} // namespace he::render
