#pragma once

// ============================================================
// ParticleRenderer — GPU 粒子系统渲染器
// ============================================================

#include "RHI/RHI.h"
#include "Scene/ParticleComponent.h"
#include "Pipeline/Camera.h"
#include <memory>
#include <vector>

namespace he::render {

class ParticleRenderer {
public:
    bool Initialize(rhi::IRHIDevice* device);
    void Shutdown(rhi::IRHIDevice* device);

    u32  RegisterComponent(ParticleComponent* comp, rhi::IRHIDevice* device);
    void UnregisterComponent(u32 id);

    void DispatchCompute(rhi::IRHICommandList* cmd, u32 id, float deltaTime);
    void Render(rhi::IRHICommandList* cmd, u32 id,
                const float4x4& viewProj, const CameraData& camera);

    rhi::IRHIBuffer* GetDrawIndirectBuffer(u32 id) const;

private:
    struct CompState {
        ParticleComponent* comp = nullptr;
        u32 maxParticles = 0;
        bool buffersCreated = false;

        // GPU Buffers
        std::unique_ptr<rhi::IRHIBuffer> particleBuf;      // Particle[maxParticles]
        std::unique_ptr<rhi::IRHIBuffer> deadList;          // u32[maxParticles]
        std::unique_ptr<rhi::IRHIBuffer> alivePre;          // u32[maxParticles]
        std::unique_ptr<rhi::IRHIBuffer> alivePost;         // u32[maxParticles]
        std::unique_ptr<rhi::IRHIBuffer> counters;          // ParticleCounters
        std::unique_ptr<rhi::IRHIBuffer> randomFloats;      // float[kRandomFloatNum]
        std::unique_ptr<rhi::IRHIBuffer> sortIndices;       // SortInfo[sortCapacity]
        std::unique_ptr<rhi::IRHIBuffer> emitUB;            // GpuEmitParam UB
        std::unique_ptr<rhi::IRHIBuffer> simUB;             // GpuSimulateParam UB
        std::unique_ptr<rhi::IRHIBuffer> cullingUB;         // GpuCullingParam UB
        std::unique_ptr<rhi::IRHIBuffer> renderUB;          // GpuRenderParam UB
        std::unique_ptr<rhi::IRHIBuffer> drawIndirectArgs;  // ParticleDrawArgs
        std::unique_ptr<rhi::IRHIBuffer> emitIndirectArgs;  // DispatchArgs
        std::unique_ptr<rhi::IRHIBuffer> simIndirectArgs;   // DispatchArgs
        std::unique_ptr<rhi::IRHIBuffer> billboardVB;       // 6×float4

        // DescriptorSets
        rhi::DescriptorSetHandle initSet  = rhi::kInvalidSet;
        rhi::DescriptorSetHandle emitSet  = rhi::kInvalidSet;
        rhi::DescriptorSetHandle simSet   = rhi::kInvalidSet;
        rhi::DescriptorSetHandle cullingSet = rhi::kInvalidSet;

        // 时间
        float elapsed = 0.0f;
        float lastEmitTime = 0.0f;

        void CreateBuffers(rhi::IRHIDevice* device, u32 sortCapacity);
    };

    // Shared PSO (每个组件共享 Shader，只有 DescriptorSet 不同)
    std::unique_ptr<rhi::IRHIPipelineState> m_InitPSO, m_EmitPSO, m_SimPSO, m_CullingPSO;
    rhi::DescriptorSetLayoutHandle m_InitLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetLayoutHandle m_EmitLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetLayoutHandle m_SimLayout  = rhi::kInvalidLayout;
    rhi::DescriptorSetLayoutHandle m_CullingLayout = rhi::kInvalidLayout;
    rhi::ShaderBytecode m_InitCS, m_EmitCS, m_SimCS, m_CullingCS;

    bool m_Initialized = false;
    rhi::IRHIDevice* m_Device = nullptr;
    std::vector<CompState> m_Components;
};

} // namespace he::render
