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

    /// GPU Tick: TickBegin → Emit → Simulate → Culling（Compute 队列）
    void DispatchCompute(rhi::IRHICommandList* cmd, u32 id, float deltaTime,
                         const float4x4& viewProj);

    /// Render: 粒子 Billboard 渲染到当前 RenderTarget
    void Render(rhi::IRHICommandList* cmd, u32 id,
                const float4x4& viewProj, const CameraData& camera);

    rhi::IRHIBuffer* GetDrawIndirectBuffer(u32 id) const;
    rhi::IRHIPipelineState* GetRenderPSO() const { return m_RenderPSO.get(); }

    /// 设置场景深度纹理（用于软粒子边缘混合）
    void SetSceneDepth(rhi::IRHITexture* depthTexture, rhi::IRHISampler* depthSampler);

    // 调试：回读 GPU 缓冲区并打印完整管线状态
    void DebugDumpState(u32 id, const char* step);

private:
    struct CompState {
        ParticleComponent* comp = nullptr;
        u32 maxParticles = 0;
        bool buffersCreated = false;
        bool initDone = false;  // Init shader 是否已执行（只执行一次）
        float elapsed = 0.0f;
        float lastEmitTime = 0.0f;
        u32 cachedRenderCount = 0;  // 缓存上一帧 GPU 输出，避免 CPU 清零后 Render 读到 0

        std::unique_ptr<rhi::IRHIBuffer> particleBuf;
        std::unique_ptr<rhi::IRHIBuffer> deadList, alivePre, alivePost;
        std::unique_ptr<rhi::IRHIBuffer> counters, randomFloats;
        std::unique_ptr<rhi::IRHIBuffer> sortIndices;
        std::unique_ptr<rhi::IRHIBuffer> emitUB, simUB, cullingUB, renderUB;
        std::unique_ptr<rhi::IRHIBuffer> drawIndirectArgs, emitIndirectArgs, simIndirectArgs;
        std::unique_ptr<rhi::IRHIBuffer> billboardVB;

        rhi::DescriptorSetHandle initSet = rhi::kInvalidSet;
        rhi::DescriptorSetHandle emitSet = rhi::kInvalidSet;
        rhi::DescriptorSetHandle simSet  = rhi::kInvalidSet;
        rhi::DescriptorSetHandle cullingSet = rhi::kInvalidSet;
        rhi::DescriptorSetHandle renderSet  = rhi::kInvalidSet;
        rhi::DescriptorSetHandle sortSet    = rhi::kInvalidSet;

        // 渐变纹理 (ColorOverLife + SizeOverLife)
        std::unique_ptr<rhi::IRHITexture>  colorOverLifeTex;
        std::unique_ptr<rhi::IRHISampler>  gradientSampler;

        // 场景深度（软粒子，由 DeferredPipeline 传入）
        rhi::IRHITexture* sceneDepthTex = nullptr;
        rhi::IRHISampler* sceneDepthSampler = nullptr;

        void CreateBuffers(rhi::IRHIDevice* device, u32 sortCapacity);
        void UpdateGradientTextures(rhi::IRHIDevice* device);
    };

    // Compute PSOs
    std::unique_ptr<rhi::IRHIPipelineState> m_InitPSO, m_EmitPSO, m_SimPSO, m_CullingPSO, m_SortPSO;
    rhi::DescriptorSetLayoutHandle m_InitLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetLayoutHandle m_EmitLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetLayoutHandle m_SimLayout  = rhi::kInvalidLayout;
    rhi::DescriptorSetLayoutHandle m_CullingLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetLayoutHandle m_SortLayout = rhi::kInvalidLayout;
    rhi::ShaderBytecode m_InitCS, m_EmitCS, m_SimCS, m_CullingCS, m_SortCS;

    // Render PSO
    std::unique_ptr<rhi::IRHIPipelineState> m_RenderPSO;
    rhi::DescriptorSetLayoutHandle m_RenderLayout = rhi::kInvalidLayout;
    rhi::ShaderBytecode m_RenderVS, m_RenderFS;

    bool m_Initialized = false;
    rhi::IRHIDevice* m_Device = nullptr;
    std::vector<CompState> m_Components;
    rhi::IRHITexture* m_SceneDepthTex = nullptr;       // 场景深度（软粒子，在 RegisterComponent 时绑定）
    rhi::IRHISampler* m_SceneDepthSampler = nullptr;
};

} // namespace he::render
