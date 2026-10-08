// ============================================================
// InstanceCuller.cpp — 逐实例 GPU 视锥剔除（任务 25）
// 阶段 1 第①段（§15.1）：逐网格实例缓冲的状态与生命周期从组件搬到本类（按 meshIndex 索引），
// 实例变换数据由 `FrameSceneSnapshot` 按值送达 ⇒ 渲染期不再读 InstancedMeshComponent。
// ============================================================

#include "Pipeline/InstanceCuller.h"
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
        // 实例缓冲状态表：连同实例 SSBO / 命令缓冲的 bindless 槽位一并释放
        // （设备仍然有效时才能回收槽位，因此这一步必须在 m_Device 置空之前）
        for (auto& kv : m_Instances) {
            InstanceState& st = kv.second;
            if (st.ssboHandle != 0) m_Device->GetBindlessHeap()->ReleaseBuffer(st.ssboHandle);
            for (u32 slot = 0; slot < rhi::kMaxFramesInFlight; ++slot) {
                if (st.cullCmdHandle[slot] != 0) {
                    m_Device->GetBindlessHeap()->ReleaseBuffer(st.cullCmdHandle[slot]);
                    st.cullCmdHandle[slot] = 0;
                }
                st.cullCmd[slot].reset();
            }
            st.retired.FlushAll();   // 设备即将销毁：不再需要 N 帧延迟
            st.buffer.reset();
        }
    }
    m_Instances.clear();
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

void InstanceCuller::BeginInstancesFrame(rhi::IRHIDevice* device) {
    for (auto it = m_Instances.begin(); it != m_Instances.end(); ) {
        InstanceState& st = it->second;

        // 「上一帧起就没再出现」⇒ 组件已销毁（或该索引不再使用）：整条状态连同 GPU 资源回收。
        // 【为什么以"上一帧"为准】快照每帧都包含全部实例化组件，本帧刚创建的条目会被
        // UploadInstanceTransforms 置为 seenThisFrame；若以"本帧未见"为准，就会把
        // "本帧还没轮到绘制"的条目误回收（顺序依赖、且渲染线程下不可复现）。
        if (!st.seenLastFrame) {
            if (device) {
                if (st.ssboHandle != 0) device->GetBindlessHeap()->ReleaseBuffer(st.ssboHandle);
                for (u32 slot = 0; slot < rhi::kMaxFramesInFlight; ++slot) {
                    if (st.cullCmdHandle[slot] != 0) {
                        device->GetBindlessHeap()->ReleaseBuffer(st.cullCmdHandle[slot]);
                    }
                }
            }
            st.retired.FlushAll();
            it = m_Instances.erase(it);
            continue;
        }

        // 帧边界推进退役队列（有界释放；先推进本帧再入队本帧退役的资源 —— 与原实现同序）
        st.retired.Advance();
        st.seenLastFrame = st.seenThisFrame;
        st.seenThisFrame = false;
        ++it;
    }
}

u32 InstanceCuller::UploadInstanceTransforms(rhi::IRHIDevice* device, u32 meshIndex,
                                             const float4x4* transforms, u32 count,
                                             u32 transformVersion, u64 ownerEntity) {
    if (!device || meshIndex == 0u) return 0;      // 0 = 未注册（与注册表的哨兵口径一致）
    if (count == 0u || !transforms) return 0;

    // 首次出现即建条目（两个 seen 都置真：本帧已见 ⇒ 下一帧的回收判定不该把它算作"未见"）
    auto [it, inserted] = m_Instances.try_emplace(meshIndex);
    InstanceState& st = it->second;
    if (inserted) {
        st.ownerEntity   = ownerEntity;
        st.seenThisFrame = true;
        st.seenLastFrame = true;
    }
    st.seenThisFrame = true;

    // 索引复用识别：同一个 meshIndex 换了来源实体 ⇒ 这是**另一个网格**，上一份缓冲的内容与
    // 版本号都不可信（旧的 `uploadedVersion` 可能恰好等于新组件的版本号 ⇒ 漏传 ⇒ 画出上一个组件的
    // 实例）。直接丢弃旧缓冲并强制重传：旧缓冲已不被任何在飞帧引用（本索引换了主人），无需延迟释放。
    if (st.ownerEntity != ownerEntity) {
        if (st.ssboHandle != 0) device->GetBindlessHeap()->ReleaseBuffer(st.ssboHandle);
        st.retired.FlushAll();
        st.buffer.reset();
        st.ssboHandle      = 0;
        st.capacity        = 0;
        st.hasUpload       = false;
        st.uploadedVersion = 0;
        st.lastCount       = 0;
        st.ownerEntity     = ownerEntity;
    }

    // 版本号没变 ⇒ 数据就是上次上传的那一份，不需要重传（跨帧幂等：同一帧被消费多次也不会重复上传）
    if (st.hasUpload && st.uploadedVersion == transformVersion) return st.ssboHandle;

    const bool needGrow = (!st.buffer || st.capacity < count);
    if (needGrow) {
        rhi::BufferDesc desc;
        desc.size        = sizeof(float4x4) * count;
        desc.usage       = rhi::BufferUsage::Storage;
        desc.initialData = transforms;
        desc.cpuAccess   = true;
        // 旧缓冲退役（N 帧延迟释放）+ 释放旧 bindless 槽位（任务 23 的槽位回收）
        if (st.buffer) {
            device->GetBindlessHeap()->ReleaseBuffer(st.ssboHandle);
            st.retired.Retire(std::move(st.buffer));
        }
        st.buffer = device->CreateBuffer(desc);
        if (!st.buffer) {
            st.ssboHandle = 0;
            st.capacity   = 0;
            st.hasUpload  = false;
            return 0;
        }
        st.capacity   = count;
        st.ssboHandle = device->GetBindlessHeap()->RegisterBuffer(st.buffer.get());
    } else {
        // 复用缓冲：Map 原地写入最新变换（容量可能大于实例数，只写前 count 个）
        void* mapped = st.buffer->Map();
        if (mapped) {
            std::memcpy(mapped, transforms, sizeof(float4x4) * count);
            st.buffer->Unmap();
        }
    }
    st.uploadedVersion = transformVersion;
    st.hasUpload       = true;
    st.lastCount       = count;
    return st.ssboHandle;
}

InstanceCuller::InstanceState* InstanceCuller::FindInstanceState(u32 meshIndex) {
    auto it = m_Instances.find(meshIndex);
    return it == m_Instances.end() ? nullptr : &it->second;
}

InstanceCuller::InstanceStats InstanceCuller::GetInstanceStats(u32 meshIndex) const {
    InstanceStats out;
    auto it = m_Instances.find(meshIndex);
    if (it == m_Instances.end()) return out;
    const InstanceState& st = it->second;
    out.valid        = st.hasUpload;
    out.ssboHandle   = st.ssboHandle;
    out.capacity     = st.capacity;
    out.retired      = st.retired.GetPendingCount();
    out.visibleCount = st.visibleInstanceCount;
    out.count        = st.lastCount;
    return out;
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
