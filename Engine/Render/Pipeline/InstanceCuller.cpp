// ============================================================
// InstanceCuller.cpp — 逐实例 GPU 视锥剔除（任务 25）
// ============================================================

#include "Pipeline/InstanceCuller.h"
#include "Scene/InstancedMeshComponent.h"   // 实例变换上传（Forward/Deferred 共用）
#include "Math/Geometry.h"   // Frustum::FromViewProj（六平面提取）
#include "Core/Log.h"

#include "InstancedCull.comp.spv.h"

#include <cstdio>
#include <cstring>

namespace he::render {

bool InstanceCuller::Initialize(rhi::IRHIDevice* device) {
    m_Device = device;
    if (!m_Device) return false;

    // 1. 共享可见实例索引列表（u32 × kMaxInstances）——每飞行帧一份
    for (u32 slot = 0; slot < rhi::kMaxFramesInFlight; ++slot) {
        rhi::BufferDesc vb;
        vb.size  = sizeof(u32) * kMaxInstances;
        vb.usage = rhi::BufferUsage::Storage;
        m_VisibleBuf[slot] = m_Device->CreateBuffer(vb);
        if (!m_VisibleBuf[slot]) {
            HE_CORE_ERROR("InstanceCuller: 可见实例列表创建失败（槽位 {}）", slot);
            return false;
        }
        m_VisibleSSBOHandle[slot] =
            m_Device->GetBindlessHeap()->RegisterBuffer(m_VisibleBuf[slot].get());
    }

    // 2. 描述符集布局：与引擎其它 pass 一致地登记 bindless 三件套
    //（本 pass 只用 binding 30 的 SSBO 数组；纹理/采样器一并登记是因为 bindless 堆的
    //  Flush 会向所有已登记 set 推送整份数组，布局里没有对应 binding 会写失败）
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        { rhi::kBindingBindlessTextures, rhi::DescriptorType::SampledImage, 4096,
          rhi::kStageMaskCompute, true },
        { rhi::kBindingBindlessSamplers, rhi::DescriptorType::Sampler, 4096,
          rhi::kStageMaskCompute, true },
        { rhi::kBindingBindlessSSBO, rhi::DescriptorType::StorageBuffer, 4096,
          rhi::kStageMaskCompute, true },
    };
    m_Layout = m_Device->CreateDescriptorSetLayout(layout);
    m_Set    = m_Device->AllocateDescriptorSet(m_Layout);
    m_Device->GetBindlessHeap()->RegisterDescriptorSet(
        m_Set, rhi::kBindingBindlessTextures, rhi::kBindingBindlessSamplers, rhi::kBindingBindlessSSBO);

    // 3. Compute PSO
    m_CS.stage = rhi::ShaderStage::Compute;
    m_CS.spirv = k_InstancedCull_comp_spv;
    m_CS.entryPoint = "main";

    rhi::PushConstantRange pc;
    pc.stageMask = rhi::kStageMaskCompute;
    pc.size      = sizeof(InstancedCullParams);

    rhi::PipelineStateDesc desc;
    desc.computeShader        = &m_CS;
    desc.bindPoint            = rhi::PipelineBindPoint::Compute;
    desc.pushConstantRanges   = { pc };
    desc.descriptorSetLayouts = { m_Layout };
    desc.debugName            = "InstancedCull";
    m_PSO = m_Device->CreatePipelineState(desc);
    if (!m_PSO) {
        HE_CORE_ERROR("InstanceCuller: compute PSO 创建失败");
        return false;
    }

    HE_CORE_INFO("InstanceCuller: 初始化完成（逐实例视锥剔除，可见列表容量 {}）", kMaxInstances);
    return true;
}

void InstanceCuller::Shutdown() {
    m_PSO.reset();
    if (m_Device) {
        for (u32 slot = 0; slot < rhi::kMaxFramesInFlight; ++slot) {
            if (m_VisibleSSBOHandle[slot] != 0) {
                m_Device->GetBindlessHeap()->ReleaseBuffer(m_VisibleSSBOHandle[slot]);  // 任务 23：槽位回收
                m_VisibleSSBOHandle[slot] = 0;
            }
            m_VisibleBuf[slot].reset();
        }
    }
    if (m_Device && m_Layout != rhi::kInvalidLayout) m_Device->DestroyDescriptorSetLayout(m_Layout);
    m_Layout = rhi::kInvalidLayout;
    m_Set    = rhi::kInvalidSet;
    m_Device = nullptr;
    m_CullCount = 0;
    m_LastInstanceCount = 0;
}

std::unique_ptr<rhi::IRHIBuffer> InstanceCuller::CreateCommandBuffer(u32 indexCount, u32 firstIndex,
                                                                     i32 vertexOffset) {
    if (!m_Device) return nullptr;
    rhi::BufferDesc desc;
    desc.size      = sizeof(InstanceIndirectCommand);
    desc.usage     = rhi::BufferUsage::Storage;
    desc.cpuAccess = true;               // CPU 填命令头 + 读回可见实例数
    auto buf = m_Device->CreateBuffer(desc);
    if (!buf) return nullptr;

    // 预填 indexCount/firstIndex/vertexOffset（每帧只有 instanceCount 会被 GPU 改写）
    InstanceIndirectCommand cmd;
    cmd.indexCount    = indexCount;
    cmd.instanceCount = 0;
    cmd.firstIndex    = firstIndex;
    cmd.vertexOffset  = vertexOffset;
    cmd.firstInstance = 0;
    if (void* p = buf->Map()) {
        std::memcpy(p, &cmd, sizeof(cmd));
        buf->Unmap();
    }
    return buf;
}

u32 InstanceCuller::UploadInstanceTransforms(rhi::IRHIDevice* device, he::InstancedMeshComponent& im) {
    if (!device) return 0;

    // 帧边界推进退役队列（有界释放；先推进本帧再入队本帧退役的资源）
    im.AdvanceRetireQueue();

    const u32 count = im.GetInstanceCount();
    if (count == 0 || im.GetIndexCount() == 0) return 0;

    if (im.bTransformsDirty || !im.instanceBuffer) {
        const bool needGrow = (!im.instanceBuffer || im.instanceBufferCapacity < count);
        if (needGrow) {
            rhi::BufferDesc desc;
            desc.size        = sizeof(float4x4) * count;
            desc.usage       = rhi::BufferUsage::Storage;
            desc.initialData = im.instanceTransforms.data();
            desc.cpuAccess   = true;
            // 旧缓冲退役（N 帧延迟释放）+ 释放旧 bindless 槽位（任务 23 的槽位回收）
            if (im.instanceBuffer) {
                device->GetBindlessHeap()->ReleaseBuffer(im.instanceSSBOHandle);
                im.RetireInstanceBuffer();
            }
            im.instanceBuffer = device->CreateBuffer(desc);
            if (!im.instanceBuffer) return 0;
            im.instanceBufferCapacity = count;
            im.instanceSSBOHandle = device->GetBindlessHeap()->RegisterBuffer(im.instanceBuffer.get());
        } else {
            // 复用缓冲：Map 原地写入最新变换（容量可能大于实例数，只写前 count 个）
            void* mapped = im.instanceBuffer->Map();
            if (mapped) {
                std::memcpy(mapped, im.instanceTransforms.data(), sizeof(float4x4) * count);
                im.instanceBuffer->Unmap();
            }
        }
        im.bTransformsDirty = false;
    }
    return im.instanceSSBOHandle;
}

u32 InstanceCuller::Cull(rhi::IRHICommandList* cmd,
                          rhi::IRHIBuffer* instanceBuffer, u32 instanceSSBOHandle,
                          rhi::IRHIBuffer* commandBuffer, u32 commandSSBOHandle,
                          u32 instanceCount, u32 frameSlot,
                          const float3& localBoundsMin, const float3& localBoundsMax,
                          const float4x4& viewProj) {
    if (!cmd || !m_PSO || !commandBuffer) return 0;
    frameSlot %= rhi::kMaxFramesInFlight;

    // 命令头复位 + **先读回**上一帧的可见数（GPU 已经写完；本帧清零之后读到的必然是 0，
    // 因为 GPU 执行是异步的 —— 这个顺序错过去就会看到"可见实例恒为 0"）
    u32 prevVisible = 0;
    if (void* p = commandBuffer->Map()) {
        auto* c = static_cast<InstanceIndirectCommand*>(p);
        prevVisible      = c->instanceCount;
        c->instanceCount = 0;
        c->firstInstance = 0;
        commandBuffer->Unmap();
    }
    m_LastInstanceCount = instanceCount;
    if (instanceCount == 0 || !instanceBuffer || m_VisibleSSBOHandle[frameSlot] == 0)
        return prevVisible;
    if (instanceCount > kMaxInstances) instanceCount = kMaxInstances;   // 容量上限，超出部分不画

    const Frustum frustum = Frustum::FromViewProj(viewProj);
    InstancedCullParams pcs{};
    for (u32 i = 0; i < 6; ++i) pcs.planes[i] = frustum.planes[i];
    pcs.localBoundsMin       = float4(localBoundsMin, 0.0f);
    pcs.localBoundsMax       = float4(localBoundsMax, 0.0f);
    pcs.instanceCount        = instanceCount;
    pcs.instanceBufferHandle = instanceSSBOHandle;
    pcs.visibleBufferHandle  = m_VisibleSSBOHandle[frameSlot];
    pcs.commandHandle        = commandSSBOHandle;

    cmd->SetPipeline(m_PSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Set);
    cmd->SetPushConstants(0, sizeof(pcs), &pcs);

    char label[64];
    snprintf(label, sizeof(label), "InstancedCull (%u inst)", instanceCount);
    cmd->SetDrawDebugLabel(label);
    cmd->Dispatch((instanceCount + 63) / 64, 1, 1);

    // 屏障：compute 写的可见列表/命令 → 之后的 DrawIndexedIndirect（读命令）与顶点着色器（读列表）
    cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader,
                         rhi::PipelineStage::DrawIndirect | rhi::PipelineStage::VertexShader,
                         rhi::ResourceState::UnorderedAccess,
                         rhi::ResourceState::ShaderResource);
    ++m_CullCount;
    return prevVisible;
}

} // namespace he::render
