// ============================================================
// RTShadowPass.cpp — 硬件 Ray Tracing 阴影 Pass 实现
// ============================================================
#include "RT/RTShadowPass.h"
#include "Core/Log.h"
#include <glm/gtc/type_ptr.hpp>

namespace he::render {

bool RTShadowPass::Initialize(rhi::IRHIDevice* device, u32 width, u32 height, bool halfRes) {
    m_FullWidth  = width;
    m_FullHeight = height;
    m_HalfRes    = halfRes;

    m_Width  = halfRes ? (width + 1) / 2 : width;
    m_Height = halfRes ? (height + 1) / 2 : height;

    // ── 创建阴影遮罩输出纹理 (R16_FLOAT) ──
    {
        rhi::TextureDesc d;
        d.format = rhi::Format::R16_FLOAT;
        d.width  = m_Width; d.height = m_Height;
        d.usage  = rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::ShaderResource;
        m_ShadowMask = device->CreateTexture(d);
    }

    // ── 创建描述符集布局 (set=0: RayGen 资源) ──
    CreateDescriptorSet(device);

    HE_CORE_INFO("RTShadowPass: 初始化完成 ({}x{}, halfRes={})",
                 m_Width, m_Height, m_HalfRes);
    return true;
}

void RTShadowPass::Shutdown() {
    m_ShadowMask.reset();
    m_Width = m_Height = 0;
}

void RTShadowPass::OnResize(u32 width, u32 height) {
    m_FullWidth  = width;
    m_FullHeight = height;
    m_Width  = m_HalfRes ? (width + 1) / 2 : width;
    m_Height = m_HalfRes ? (height + 1) / 2 : height;
    // 纹理重建由调用方通过 Shutdown + Initialize 处理
}

void RTShadowPass::Execute(rhi::IRHICommandList* cmd,
                            rhi::IRHIAccelerationStructure* tlas,
                            rhi::IRHITexture* gbDepth,
                            rhi::IRHITexture* gbNormal,
                            rhi::IRHIBuffer*  lightBuffer,
                            u32 lightCount,
                            const float4x4& invViewProj,
                            const float3& cameraPos,
                            u32 frameIndex) {
    if (!m_ShadowMask || !tlas) return;

    // ── 更新描述符集 ──
    // Binding 0: TLAS (AccelerationStructure)
    // Binding 1: ShadowMask (StorageImage — 输出)
    // Binding 2: GBuffer Depth (SampledImage)
    // Binding 3: GBuffer Normal (SampledImage)
    // Binding 4: Light Buffer (UniformBuffer / StorageBuffer)

    // 注意: 实际的描述符更新需要 RHI 接口支持。当前版本使用 RTPass
    // 的 UpdateRTDescriptorSet 来完成。此方法预留 Phase 4 完善。

    // ── 更新 push constants ──
    struct {
        float invViewProj[16];
        float camPos[4];
        u32   lightCount;
        u32   shadowFlags;
        u32   frameIdx;
        u32   pad;
    } pc;
    memcpy(pc.invViewProj, glm::value_ptr(invViewProj), sizeof(float) * 16);
    pc.camPos[0] = cameraPos.x; pc.camPos[1] = cameraPos.y;
    pc.camPos[2] = cameraPos.z; pc.camPos[3] = 1.0f;
    pc.lightCount = lightCount;
    pc.shadowFlags = m_HalfRes ? 2 : 0;
    pc.frameIdx = frameIndex;

    cmd->SetPushConstants(0, sizeof(pc), &pc);

    // 注意: 实际 TraceRays 调用由 HybridRTPipeline 通过 RTPass 完成
    // 此处仅设置 push constants 和描述符集
    (void)gbDepth; (void)gbNormal; (void)lightBuffer;
}

void RTShadowPass::CreateDescriptorSet(rhi::IRHIDevice* device) {
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        {0, rhi::DescriptorType::AccelerationStructure, 1, rhi::kStageMaskRayGen},
        {1, rhi::DescriptorType::StorageImage, 1, rhi::kStageMaskRayGen},
        {2, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskRayGen},
        {3, rhi::DescriptorType::SampledImage, 1, rhi::kStageMaskRayGen},
        {4, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskRayGen},
    };
    m_Layout = device->CreateDescriptorSetLayout(layout);
    m_Set    = device->AllocateDescriptorSet(m_Layout);

    m_PCRange.stageMask = rhi::kStageMaskRayGen;
    m_PCRange.size      = 128;  // 4x4 float + 4 + 3x uint
}

} // namespace he::render
