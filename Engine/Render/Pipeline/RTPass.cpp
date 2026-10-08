// ============================================================
// RTPass.cpp — Ray Tracing Pass 管理器实现
// 负责 BLAS/TLAS 构建、RT PSO 管理、TraceRays 调度
// ============================================================
#include "Pipeline/RTPass.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/MeshComponent.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Core/Log.h"
#include "Core/Assert.h"
#include "RT/PTMaterialParams.h"   // Disney 参数打包（与光栅化 CPU 侧、PT 载荷同源）

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>   // std::log（介质吸收系数 σ_t 推导）
#include <cstring>

namespace he::render {

RTPass::RTPass() = default;

RTPass::~RTPass() {
    Shutdown();
}

bool RTPass::Initialize(rhi::IRHIDevice* device,
                         const std::vector<rhi::ShaderBytecode>& rtShaders,
                         const std::vector<rhi::RTShaderGroup>& shaderGroups,
                         const std::vector<rhi::DescriptorSetLayoutHandle>& descLayouts,
                         rhi::PushConstantRange pushConstRange) {
    m_Device = device;
    m_Shaders = rtShaders;
    m_ShaderGroups = shaderGroups;
    m_PushConstRange = pushConstRange;

    if (!descLayouts.empty()) m_DescLayout = descLayouts[0];
    if (descLayouts.size() > 1) m_DescLayout1 = descLayouts[1];

    // 检查设备 RT 支持
    auto caps = device->GetCaps();
    if (!caps.supportsRayTracing) {
        HE_CORE_WARN("RTPass: 设备不支持 Ray Tracing，初始化跳过");
        return false;
    }

    // 0. 分配描述符集
    if (m_DescLayout != rhi::kInvalidLayout) {
        m_DescSet = device->AllocateDescriptorSet(m_DescLayout);
    }
    if (m_DescLayout1 != rhi::kInvalidLayout) {
        m_DescSet1 = device->AllocateDescriptorSet(m_DescLayout1);
    }
    if (m_DescLayout2 != rhi::kInvalidLayout) {
        m_DescSet2 = device->AllocateDescriptorSet(m_DescLayout2);
    }

    // 1. 创建 RT Pipeline State（AS-only 模式：rtShaders 为空则跳过管线+SBT，
    //    仅构建 BLAS/TLAS——各 RT 效果的管线由 RTEffectPass 各自创建）
    if (!rtShaders.empty()) {
        rhi::RTPipelineStateDesc rtpDesc;
        rtpDesc.shaders        = rtShaders;
        rtpDesc.shaderGroups   = shaderGroups;
        rtpDesc.maxRecursionDepth = rhi::kRTMaxRecursionDepth;  // RayGen(0) → ClosestHit(1) → Callable(2)
        rtpDesc.maxPayloadSize    = rhi::kRTMaxPayloadSize;
        rtpDesc.maxHitAttributeSize = rhi::kRTMaxHitAttributeSize;
        rtpDesc.debugName      = "RTPass";
        for (auto& l : descLayouts) {
            if (l != rhi::kInvalidLayout)
                rtpDesc.descriptorSetLayouts.push_back(l);
        }
        // 如果 set=2 已创建（bindless），也加入布局
        if (m_DescLayout2 != rhi::kInvalidLayout)
            rtpDesc.descriptorSetLayouts.push_back(m_DescLayout2);
        if (m_PushConstRange.size > 0)
            rtpDesc.pushConstantRanges.push_back(m_PushConstRange);

        m_RTPipeline = device->CreateRTPipelineState(rtpDesc);
        if (!m_RTPipeline) {
            HE_CORE_ERROR("RTPass: RT Pipeline State 创建失败");
            return false;
        }

        // 2. 创建 SBT
        if (!CreateSBT(device)) {
            HE_CORE_ERROR("RTPass: SBT 创建失败");
            return false;
        }
    } else {
        HE_CORE_INFO("RTPass: AS-only 模式（无独立管线，效果管线由 RTEffectPass 创建）");
    }

    // 3. 预创建 TLAS（构建在 BuildAS 中完成）
    rhi::TLASBuildDesc tlasDesc;
    tlasDesc.maxInstanceCount = m_MaxInstanceCount;
    tlasDesc.flags = rhi::ASBuildFlags::PreferFastTrace;
    m_TLAS = device->CreateTLAS(tlasDesc);
    if (!m_TLAS) {
        HE_CORE_ERROR("RTPass: TLAS 创建失败");
        return false;
    }

    // 4. 分配 TLAS Scratch Buffer + Instance Buffer
    rhi::ASBuildSizes tlasSizes = device->GetTLASBuildSizes(m_MaxInstanceCount);
    {
        rhi::BufferDesc sb;
        sb.size  = tlasSizes.buildScratchSize;
        sb.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::AccelerationStruct;
        m_TLASScratch = device->CreateBuffer(sb);
    }
    {
        rhi::BufferDesc ib;
        ib.size  = sizeof(rhi::TLASInstanceDesc) * m_MaxInstanceCount;
        ib.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::AccelerationStruct;
        m_TLASInstanceBuffer = device->CreateBuffer(ib);
    }

    m_Initialized = true;
    HE_CORE_INFO("RTPass: 初始化完成 (maxInstances={})", m_MaxInstanceCount);
    return true;
}

// ============================================================
// BuildSBT — 从 RT PSO 句柄填充 Shader Binding Table（静态辅助）
// 被 CreateSBT 与 CreateEffectPipeline 共用
// ============================================================
static bool BuildSBT(rhi::IRHIDevice* device,
                     rhi::IRHIRayTracingPipelineState* pipeline,
                     const std::vector<rhi::RTShaderGroup>& shaderGroups,
                     rhi::SBTDesc& outSBT,
                     std::unique_ptr<rhi::IRHIBuffer>& outBuffer) {
    if (!pipeline) return false;

    u32 groupCount   = pipeline->GetShaderGroupCount();
    u32 handleSize   = pipeline->GetShaderGroupHandleSize();
    auto handles     = pipeline->GetShaderGroupHandles();

    u32 align = device->GetCaps().shaderGroupBaseAlignment;
    auto aligned = [align](u32 size) -> u32 {
        return (size + align - 1) & ~(align - 1);
    };

    // ── 分类统计各组类型 ──
    std::vector<u32> rgIdx, missIdx, hitIdx, callIdx;
    for (u32 g = 0; g < groupCount; ++g) {
        auto t = shaderGroups[g].type;
        if (t == rhi::RTShaderGroupType::RayGen)      rgIdx.push_back(g);
        else if (t == rhi::RTShaderGroupType::Miss)    missIdx.push_back(g);
        else if (t == rhi::RTShaderGroupType::Hit)     hitIdx.push_back(g);
        else if (t == rhi::RTShaderGroupType::Callable) callIdx.push_back(g);
    }

    u32 stride = aligned(handleSize);
    u32 rgOff  = 0;
    u32 msOff  = rgOff  + stride * (u32)rgIdx.size();
    u32 htOff  = msOff  + stride * (u32)missIdx.size();
    u32 caOff  = htOff  + stride * (u32)hitIdx.size();
    u32 total  = caOff  + stride * (u32)callIdx.size();
    if (total == 0) return false;

    rhi::BufferDesc sbtDesc;
    sbtDesc.size  = total;
    // SBT 缓冲必须带 VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR，
    // 否则 vkCmdTraceRaysKHR 的 deviceAddress 无有效缓冲（Validation 报错）
    sbtDesc.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::AccelerationStruct
                  | rhi::BufferUsage::Uniform | rhi::BufferUsage::ShaderBindingTable;
    outBuffer = device->CreateBuffer(sbtDesc);

    u8* mapped = static_cast<u8*>(outBuffer->Map());
    if (!mapped) return false;

    for (u32 i = 0; i < rgIdx.size(); ++i)
        std::memcpy(mapped + rgOff + i * stride, handles.data() + rgIdx[i] * handleSize, handleSize);
    for (u32 i = 0; i < missIdx.size(); ++i)
        std::memcpy(mapped + msOff + i * stride, handles.data() + missIdx[i] * handleSize, handleSize);
    for (u32 i = 0; i < hitIdx.size(); ++i)
        std::memcpy(mapped + htOff + i * stride, handles.data() + hitIdx[i] * handleSize, handleSize);
    for (u32 i = 0; i < callIdx.size(); ++i)
        std::memcpy(mapped + caOff + i * stride, handles.data() + callIdx[i] * handleSize, handleSize);
    outBuffer->Unmap();

    outSBT.buffer = outBuffer.get();
    outSBT.rayGen.handleOffset  = rgOff;
    outSBT.rayGen.stride        = rgIdx.empty()  ? 0 : stride;
    outSBT.miss.handleOffset    = msOff;
    outSBT.miss.stride          = missIdx.empty()? 0 : stride;
    outSBT.hit.handleOffset     = htOff;
    outSBT.hit.stride           = hitIdx.empty() ? 0 : stride;
    outSBT.callable.handleOffset = caOff;
    outSBT.callable.stride       = callIdx.empty()? 0 : stride;

    HE_CORE_INFO("RTPass: SBT 创建完成 ({} groups: {}RG {}Miss {}Hit {}Call, {}B)",
                 groupCount, rgIdx.size(), missIdx.size(), hitIdx.size(), callIdx.size(), total);
    return true;
}

bool RTPass::CreateSBT(rhi::IRHIDevice* device) {
    if (!m_RTPipeline) return false;
    return BuildSBT(device, m_RTPipeline.get(), m_ShaderGroups, m_SBT, m_SBTBuffer);
}

// ============================================================
// CreateEffectPipeline — 创建独立 RT 效果管线 + SBT（Deferred 的 RT 效果使用）
// ============================================================
RTPass::RTEffectPipeline RTPass::CreateEffectPipeline(
    rhi::IRHIDevice* device,
    const std::vector<rhi::ShaderBytecode>& rtShaders,
    const std::vector<rhi::RTShaderGroup>& shaderGroups,
    const std::vector<rhi::DescriptorSetLayoutHandle>& descLayouts,
    rhi::PushConstantRange pushConstRange,
    u32 maxPayloadSize,
    u32 maxRecursionDepth,
    StringView debugName) {
    RTEffectPipeline result;

    // ── 创建 RT Pipeline State ──
    rhi::RTPipelineStateDesc rtpDesc;
    rtpDesc.shaders        = rtShaders;
    rtpDesc.shaderGroups   = shaderGroups;
    rtpDesc.maxRecursionDepth = maxRecursionDepth;
    rtpDesc.maxPayloadSize    = maxPayloadSize;
    rtpDesc.maxHitAttributeSize = rhi::kRTMaxHitAttributeSize;
    rtpDesc.debugName       = String(debugName);
    for (auto& l : descLayouts) {
        if (l != rhi::kInvalidLayout)
            rtpDesc.descriptorSetLayouts.push_back(l);
    }
    if (pushConstRange.size > 0)
        rtpDesc.pushConstantRanges.push_back(pushConstRange);

    result.pipeline = device->CreateRTPipelineState(rtpDesc);
    if (!result.pipeline) {
        HE_CORE_ERROR("RTPass::CreateEffectPipeline: RT PSO 创建失败 ({})", debugName);
        return result;
    }

    // ── 创建 SBT ──
    if (!BuildSBT(device, result.pipeline.get(), shaderGroups, result.sbt, result.sbtBuffer)) {
        HE_CORE_ERROR("RTPass::CreateEffectPipeline: SBT 创建失败 ({})", debugName);
        result.pipeline.reset();
        return result;
    }

    HE_CORE_INFO("RTPass::CreateEffectPipeline: {} 管线+SBT 创建完成", debugName);
    return result;
}

void RTPass::Shutdown() {
    m_VertexPullBuffer.reset();
    m_IndexPullBuffer.reset();
    m_MaterialTex.reset();
    m_SceneMaterialTex.reset();
    m_SceneTriangleNormals.reset();
    m_SceneTriangleUVs.reset();
    m_LightUB.reset();
    m_BindlessSampler.reset();
    m_BindlessTextures.clear();
    m_BindlessSamplers.clear();
    m_TLASScratch.reset();
    m_TLASInstanceBuffer.reset();
    m_SBTBuffer.reset();
    m_RTPipeline.reset();
    m_TLAS.reset();
    m_BLASMap.clear();
    m_Initialized = false;
}

// ============================================================
// AS 构建
// ============================================================

// 【第③段第 5 批】几何哈希改吃**注册表条目**（顶点/索引数与缓冲设备地址）：
// 与旧实现同口径（顶点数 + 索引数 + 两个缓冲的设备地址），只是数据来源从组件换成注册表。
static u64 HashGeometry(const MeshRegistryEntry& e) {
    u64 h = e.vertexCount;
    h = h * 31 + e.indexCount;
    if (e.vertexBuffer) h = h * 31 + e.vertexBuffer->GetDeviceAddress();
    if (e.indexBuffer)  h = h * 31 + e.indexBuffer->GetDeviceAddress();
    return h;
}

bool RTPass::HasGeometryChanged(u32 meshIndex, const MeshRegistryEntry& entry) {
    auto it = m_BLASMap.find(meshIndex);
    if (it == m_BLASMap.end()) return true;
    return it->second.geometryHash != HashGeometry(entry);
}

// 收集本帧 **RT 可见**网格（`Base`/`Cube`/`Sphere`）——与旧实现遍历
// `MeshComponent`/`Cube`/`Sphere` 的精确类型范围**逐条一致**（顺序也一致：快照按
// Mesh → Cube → Sphere → … 枚举）。返回的是指向快照条目的指针（调用期间快照不变 ⇒ 稳定）。
static void CollectMeshList(const FrameSceneSnapshot& snapshot,
                            const MeshRegistry& registry,
                            std::vector<const SnapshotDrawItem*>& out) {
    for (const SnapshotDrawItem& it : snapshot.draws) {
        if (it.meshClass != SnapshotMeshClass::Base &&
            it.meshClass != SnapshotMeshClass::Cube &&
            it.meshClass != SnapshotMeshClass::Sphere) continue;
        if (!registry.Find(it.meshIndex)) continue;   // 未注册（无索引缓冲）⇒ 与旧口径一致地跳过
        out.push_back(&it);
    }
}

// float4x4 → float3x4 行主序变换（Vulkan VkTransformMatrixKHR 格式）
static float3x4 ToTransformMatrix(const float4x4& m) {
    // float3x4 有 3 列，每列 float4（= 3 行 × 4 元素的行主序矩阵）
    // VkTransformMatrixKHR = float[3][4] 行主序
    // t[i] 是第 i 行（作为 float4）
    float3x4 t;
    t[0] = float4(m[0][0], m[1][0], m[2][0], m[3][0]);  // 行 0: x,y,z,tx
    t[1] = float4(m[0][1], m[1][1], m[2][1], m[3][1]);  // 行 1: x,y,z,ty
    t[2] = float4(m[0][2], m[1][2], m[2][2], m[3][2]);  // 行 2: x,y,z,tz
    return t;
}

void RTPass::BuildAS(rhi::IRHICommandList* cmd, const FrameSceneSnapshot& snapshot,
                     const MeshRegistry& registry) {
    if (!m_Initialized) return;

    std::vector<const SnapshotDrawItem*> meshList;
    CollectMeshList(snapshot, registry, meshList);

    // ── 阶段 1: 构建/更新 BLAS（按 meshIndex 缓存）──
    u32 blasIdx = 0;
    for (const SnapshotDrawItem* item : meshList) {
        const MeshRegistryEntry* me = registry.Find(item->meshIndex);
        if (!me || !me->vertexBuffer || !me->indexBuffer) continue;

        if (!HasGeometryChanged(item->meshIndex, *me)) continue;   // 几何未变，跳过重建

        // 创建或复用 BLAS entry（键 = meshIndex：组件指针不参与）
        auto& entry = m_BLASMap[item->meshIndex];

        rhi::BLASBuildDesc blasDesc;
        blasDesc.flags = rhi::ASBuildFlags::PreferFastTrace;
        rhi::RTGeometryDesc geo;
        geo.type         = rhi::RTGeometryType::Triangles;
        geo.vertexBuffer = me->vertexBuffer;
        geo.vertexFormat = rhi::Format::RGB32_FLOAT;
        geo.vertexStride = sizeof(he::StaticVertex);
        geo.maxVertex    = me->vertexCount;
        geo.indexBuffer  = me->indexBuffer;
        geo.indexFormat  = rhi::Format::R32_UINT;
        geo.maxPrimitiveCount = me->indexCount / 3;
        blasDesc.geometries.push_back(geo);

        rhi::ASBuildSizes sizes = m_Device->GetBLASBuildSizes(blasDesc);

        if (!entry.blas) {
            entry.blas = m_Device->CreateBLAS(blasDesc);
        }

        // 分配/重分配 Scratch Buffer
        if (!entry.scratchBuffer || entry.scratchBuffer->GetSize() < sizes.buildScratchSize) {
            rhi::BufferDesc sb;
            sb.size  = sizes.buildScratchSize;
            sb.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::AccelerationStruct;
            entry.scratchBuffer = m_Device->CreateBuffer(sb);
        }

        cmd->BuildBLAS(entry.blas.get(), entry.scratchBuffer.get(), blasDesc, false);

        entry.geometryHash = HashGeometry(*me);
        HE_CORE_INFO("RTPass: BLAS 构建 (mesh#{}, vertices={}, triangles={}, size={}KB)",
                     blasIdx++, me->vertexCount, me->indexCount / 3,
                     sizes.accelerationStructureSize / 1024);
    }

    // ── 阶段 2: 构建 TLAS（每帧）──
    std::vector<rhi::TLASInstanceDesc> instances;
    u32 instanceID = 0;

    for (const SnapshotDrawItem* item : meshList) {
        auto it = m_BLASMap.find(item->meshIndex);
        if (it == m_BLASMap.end()) continue;

        // 世界矩阵直接取快照（旧实现走 `sg.GetWorldMatrix(entity)`，同一份数据）
        rhi::TLASInstanceDesc inst;
        inst.transform    = ToTransformMatrix(item->object.worldMatrix);
        inst.instanceID   = instanceID++;      // 实例自定义 ID（与材质纹理列索引一致）
        inst.instanceMask = 0xFF;              // 所有光线可见
        inst.sbtOffset    = 0;                 // 命中组索引（简单场景：0）
        inst.flags        = 0;
        inst.blasAddress  = it->second.blas->GetDeviceAddress();
        instances.push_back(inst);
    }

    if (!instances.empty()) {
        void* mapped = m_TLASInstanceBuffer->Map();
        std::memcpy(mapped, instances.data(), instances.size() * sizeof(rhi::TLASInstanceDesc));
        m_TLASInstanceBuffer->Unmap();

        cmd->BuildTLAS(m_TLAS.get(), m_TLASScratch.get(), m_TLASInstanceBuffer.get(),
                       static_cast<u32>(instances.size()), false);

        HE_CORE_INFO("RTPass: TLAS 构建 ({} instances)", instances.size());
    }
}

// ============================================================
// RT 管线绑定 + 调度
// ============================================================

void RTPass::BindPipeline(rhi::IRHICommandList* cmd) {
    if (!m_RTPipeline) return;
    cmd->BindRTPipeline(m_RTPipeline.get());
}

void RTPass::TraceRays(rhi::IRHICommandList* cmd, u32 width, u32 height) {
    if (!m_RTPipeline) return;
    cmd->TraceRays(m_SBT, width, height, 1);
}

// UpdateRTDescriptorSet — 每帧刷新描述符集
void RTPass::UpdateRTDescriptorSet(rhi::IRHIDevice* device,
                                    void* backBufferView,
                                    rhi::IRHIBuffer* objectDataBuffer) {
    if (!m_TLAS) return;

    // set=0: TLAS + BackBuffer（RayGen 使用）
    if (m_DescSet != rhi::kInvalidSet) {
        device->UpdateDescriptorSet(m_DescSet, 0,
            rhi::DescriptorType::AccelerationStructure, m_TLAS.get());
        device->UpdateDescriptorSetWithImageView(m_DescSet, 1,
            rhi::DescriptorType::StorageImage, backBufferView);
    }

    // set=1: 材质纹理 + 光源 UB（ClosestHit 使用）
    if (m_DescSet1 != rhi::kInvalidSet) {
        if (m_MaterialTex) {
            // 材质 = SampledImage（Texture2D::Load 不需要采样器）
            device->UpdateDescriptorSet(m_DescSet1, 0,
                rhi::DescriptorType::SampledImage,
                m_MaterialTex.get(), nullptr);
        }
        if (m_LightUB)
            device->UpdateDescriptorSet(m_DescSet1, 1,
                rhi::DescriptorType::UniformBuffer, m_LightUB.get());
    }
}

// 创建材质纹理（3×N RGBA32F），从 World MeshComponent 读取完整 PBR 材质数据
// Row 0: baseColorFactor (RGBA)
// Row 1: metallic, roughness, ao, alphaCutoff (RGBA)
// Row 2: materialID (uint as float), materialFlags (uint as float), 0, 0 (RGBA)
bool RTPass::CreateMaterialTexture(rhi::IRHIDevice* device, u32 maxInstances,
                                    const FrameSceneSnapshot& snapshot) {
    if (!device || maxInstances == 0) return false;
    m_MaterialInstanceCount = std::min(maxInstances, 256u);

    // 3 行 × N 列，默认白色兜底
    std::vector<float> texData(m_MaterialInstanceCount * 4 * 3, 1.0f);

    // 材质字段取自快照条目的 `GPUObjectData`（收集侧用 `MakePBRMaterial` + `FillObjectData` 算好，
    // 与 GBuffer 路径**同源**）；列索引与 `BuildAS` 的 TLAS 实例顺序一致。
    u32 idx = 0;
    for (const SnapshotDrawItem& it : snapshot.draws) {
        if (it.meshClass != SnapshotMeshClass::Base &&
            it.meshClass != SnapshotMeshClass::Cube &&
            it.meshClass != SnapshotMeshClass::Sphere) continue;
        if (idx >= m_MaterialInstanceCount) break;
        const GPUObjectData& o = it.object;
        float* row0 = &texData[idx * 4];
        row0[0] = o.baseColorFactor.x;
        row0[1] = o.baseColorFactor.y;
        row0[2] = o.baseColorFactor.z;
        row0[3] = o.baseColorFactor.w;
        float* row1 = &texData[m_MaterialInstanceCount * 4 + idx * 4];
        row1[0] = o.metallicFactor;
        row1[1] = o.roughnessFactor;
        row1[2] = o.aoFactor;
        row1[3] = o.alphaCutoff;
        float* row2 = &texData[m_MaterialInstanceCount * 4 * 2 + idx * 4];
        row2[0] = static_cast<float>(o.materialID);   // uint→float 值转换（避免 denormal flush-to-zero）
        row2[1] = 0.0f;
        row2[2] = 0.0f;
        row2[3] = 0.0f;
        idx++;
    }

    rhi::TextureDesc texDesc;
    texDesc.format = rhi::Format::RGBA32_FLOAT;
    texDesc.width  = m_MaterialInstanceCount;
    texDesc.height = 3;  // Phase 5: 3 行（baseColor + PBR + materialID）
    texDesc.mipLevels = 1;
    texDesc.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::TransferDst;
    texDesc.initialData = texData.data();
    m_MaterialTex = device->CreateTexture(texDesc);
    HE_CORE_INFO("RTPass: 材质纹理创建 ({}×3 RGBA32F, {} meshes)", m_MaterialInstanceCount, idx);
    return m_MaterialTex != nullptr;
}

// ============================================================
// BuildSceneMaterialTexture — 场景材质纹理（7×N）+ 三角形法线纹理
// 供 RT 反射/GI/PT 的 ClosestHit 用 InstanceID() 查询材质、
// PrimitiveIndex() 查询三角形顶点法线（重心插值 → 平滑法线）。
// 列索引与 BuildAS 的 TLAS 实例顺序一致（Mesh → Cube → Sphere）。
// 用纹理而非 SSBO：ClosestHitKHR 中访问 StructuredBuffer 已知 GPU fault；
// 不依赖 position_fetch：GTX 1070 等设备不支持 VK_KHR_ray_tracing_position_fetch。
// 行布局（每实例一列）：row0=albedo.rgb+metallic, row1=roughness+ao,
//   row2=(法线线性起始=tri*3, 三角形数, 法线纹理宽度, 0), row3=emissive.rgb+0,
//   row4=disneyA(aniso, subsurface, specular, sheen),
//   row5=disneyB(clearcoat, clearcoatGloss, specularTint.rg),
//   row6=(disneyC=specularTint.b, dielectricF0, ior, transmission),
//   row7=(σ_t.rgb, 0) —— 参与介质的 Beer-Lambert 吸收系数（由 attenuationColor /
//        attenuationDistance 推导；不吸收时为 0）
// 行 4~7 与 Material.h 的 disneyA/disneyB/disneyC 打包逐字段一致，
// 供路径追踪的 PathPayload（PT 任务 1 / 4）带上完整 Disney 参数与介质参数。
// ============================================================
bool RTPass::BuildSceneMaterialTexture(rhi::IRHIDevice* device, const FrameSceneSnapshot& snapshot,
                                       const MeshRegistry& registry) {
    if (!device) return false;

    // 收集 RT 可见网格（与 BuildAS 的实例顺序一致；第③段第 5 批：改从快照 + 注册表取）
    std::vector<const SnapshotDrawItem*> meshList;
    CollectMeshList(snapshot, registry, meshList);
    if (meshList.empty()) {
        HE_CORE_WARN("RTPass: BuildSceneMaterialTexture — 场景无网格，跳过");
        return false;
    }

    u32 n = (u32)meshList.size();
    u64 totalTris = 0;
    for (const SnapshotDrawItem* d : meshList) {
        const MeshRegistryEntry* re = registry.Find(d->meshIndex);
        if (re) totalTris += re->indexCount / 3;
    }

    // ── 材质纹理数据（11 行 × N 列）──
    // row0=albedo.rgb+metallic, row1=roughness+ao,
    // row2=(法线线性起始=tri*3, 三角形数, 法线纹理宽度, 0), row3=emissive.rgb+0,
    // row4=disneyA, row5=disneyB, row6=(disneyC, dielectricF0, ior, transmission),
    // row7=(σ_t.rgb, 0) 介质吸收系数，
    // row8=(materialID, textureMask, 0, 0) —— bindless 贴图基索引 + 纹理存在位掩码，
    // row9=(baseColorFactor.rgb, metallicFactor), row10=(roughnessFactor, 0, 0, 0)
    //   row9/row10 是「因子」：采样到真实贴图时要用 factor × texture（row0/row1 存的是
    //   贴图均值 × 因子，只在没有贴图时作为回落，直接乘贴图会把贴图算两遍）。
    std::vector<float> matData(n * 11 * 4, 0.0f);

    // ── 三角形顶点属性扁平数组（每三角形 3 条，跨所有实例）──
    // 2D 纹理布局：width=W, height=kNormTexHeight；线性索引 lin → (row=lin/W, col=lin%W)
    constexpr u32 kNormTexHeight = 1024;
    u64 entries = totalTris * 3;
    u32 normTexWidth = (u32)((entries + kNormTexHeight - 1) / kNormTexHeight);
    if (normTexWidth == 0) normTexWidth = 1;
    std::vector<float> normalData((u64)normTexWidth * kNormTexHeight * 4, 0.0f);
    // UV 与法线同布局（RG32F：xy = uv，zw 未用）——PT 的 ClosestHit 靠它插值命中点 UV
    std::vector<float> uvData((u64)normTexWidth * kNormTexHeight * 2, 0.0f);

    // ── 顶点缓冲布局（用 offsetof 适配 32B/48B 两种 GLM 布局）──
    constexpr size_t kStride  = sizeof(he::StaticVertex);
    constexpr size_t kNormOff = offsetof(he::StaticVertex, normal);
    constexpr size_t kUVOff   = offsetof(he::StaticVertex, uv);

    u64 triFlat = 0;  // 跨实例的扁平三角形索引
    for (u32 i = 0; i < n; ++i) {
        const SnapshotDrawItem& item = *meshList[i];
        const SnapshotRTMaterial& rt = item.rtMaterial;
        const GPUObjectData& obj = item.object;
        const MeshRegistryEntry* me = registry.Find(item.meshIndex);
        u32 triCount = me ? me->indexCount / 3 : 0u;

        // 材质纹理：4 行基础 PBR + Disney/介质 4 行 + 贴图索引/因子 3 行
        // 没有均值时退回因子（与光栅化的因子语义一致）。
        // 不这么做的话，像 Sponza 这种 metallicFactor 缺省 1.0、实际靠
        // metallicRoughness 贴图调制成石头的场景会被整体判成纯金属，漫反射全灭。
        // （PT 任务 5 起 PT 会真正采样贴图，此时用 row9/row10 的因子；row0/row1 的均值
        //   只在没有贴图/贴图未注册时作为回落。）
        float* row0 = &matData[i * 4];
        row0[0] = rt.hasMaterialAvg ? rt.baseColorAvg.x : obj.baseColorFactor.x;
        row0[1] = rt.hasMaterialAvg ? rt.baseColorAvg.y : obj.baseColorFactor.y;
        row0[2] = rt.hasMaterialAvg ? rt.baseColorAvg.z : obj.baseColorFactor.z;
        row0[3] = rt.hasMaterialAvg ? rt.metallicAvg  : obj.metallicFactor;
        float* row1 = &matData[n * 4 + i * 4];
        row1[0] = rt.hasMaterialAvg ? rt.roughnessAvg : obj.roughnessFactor;
        row1[1] = obj.aoFactor;
        row1[2] = 0.0f;
        row1[3] = 0.0f;
        float* row2 = &matData[n * 8 + i * 4];
        row2[0] = float(triFlat * 3);      // 本实例法线数组线性起始
        row2[1] = float(triCount);
        row2[2] = float(normTexWidth);     // 法线纹理宽度（shader 用 lin%W 计算坐标）
        row2[3] = 0.0f;
        float* row3 = &matData[n * 12 + i * 4];
        row3[0] = obj.emissiveFactor.x;
        row3[1] = obj.emissiveFactor.y;
        row3[2] = obj.emissiveFactor.z;
        row3[3] = 0.0f;
        // Disney 参数（统一走 RT/PTMaterialParams.h 的打包规则：
        // 与 Material.h 的 disneyA/disneyB/disneyC、PT 载荷逐字段同源）
        // Disney 参数与介质参数：`GPUObjectData` 里的 disneyA/B/C 与 dielectricF0 是收集侧用
        // 同一个 `PackDisneyParams` 打包的（与 Material.h 逐字段同源）⇒ 这里直接取快照打包值；
        // `ior` / `transmission` 不在 GPUObjectData 里，取 `SnapshotRTMaterial`（同一批字段）。
        const PTMaterialParams disney = [&] {
            PTMaterialParams p{};
            p.disneyA = obj.disneyA;
            p.disneyB = obj.disneyB;
            p.surfaceParams = float4(obj.disneyC, obj.dielectricF0, rt.ior, rt.transmission);
            return p;
        }();
        float* row4 = &matData[n * 16 + i * 4];
        row4[0] = disney.disneyA.x;
        row4[1] = disney.disneyA.y;
        row4[2] = disney.disneyA.z;
        row4[3] = disney.disneyA.w;
        float* row5 = &matData[n * 20 + i * 4];
        row5[0] = disney.disneyB.x;
        row5[1] = disney.disneyB.y;
        row5[2] = disney.disneyB.z;
        row5[3] = disney.disneyB.w;
        float* row6 = &matData[n * 24 + i * 4];
        row6[0] = disney.surfaceParams.x;   // disneyC = specularTint.b
        row6[1] = disney.surfaceParams.y;   // dielectricF0（由 IOR 推导）
        row6[2] = disney.surfaceParams.z;   // ior
        row6[3] = rt.transmission;          // 透射（>0 时 PT 走折射/介质分支）
        // 介质吸收系数 σ_t：Beer-Lambert 透射率 = exp(-σ_t · d)
        //   σ_t = -ln(attenuationColor) / attenuationDistance（逐通道）
        // attenuationDistance<=0（glTF 的 +inf）或颜色为 1（不吸收）时为 0 = 不衰减
        float* row7 = &matData[n * 28 + i * 4];
        row7[0] = row7[1] = row7[2] = row7[3] = 0.0f;
        if (rt.attenuationDistance > 0.0f) {
            const float inv = 1.0f / rt.attenuationDistance;
            row7[0] = -std::log(std::max(rt.attenuationColor.x, 1e-6f)) * inv;
            row7[1] = -std::log(std::max(rt.attenuationColor.y, 1e-6f)) * inv;
            row7[2] = -std::log(std::max(rt.attenuationColor.z, 1e-6f)) * inv;
        }
        // row8：bindless 贴图基索引 + 纹理存在位掩码（与光栅化路径同一套规则：
        //   Material.h::ComputeMaterialTextureMask / kGPUMaterialTexMask_*）
        //   textureMask 的位序必须与 kGPUMaterialTexSlot_* 一致（BaseColor=0, Normal=1,
        //   MetallicRough=2, Occlusion=3），采样时用 texBase + 槽号。
        // 纹理存在位掩码：直接取快照（收集侧 `MakePBRMaterial` + `FillObjectData` 已按同一套位序
        // 压好 —— bit0=BaseColor / bit1=Normal / bit2=MetallicRough / bit3=Occlusion）
        const u32 texMask = obj.textureMask;
        float* row8 = &matData[n * 32 + i * 4];
        row8[0] = static_cast<float>(obj.materialID);   // 整数按 float 存（< 2^24 无精度损失）
        row8[1] = static_cast<float>(texMask);
        row8[2] = 0.0f;
        row8[3] = 0.0f;
        // row9 / row10：材质因子（采样到真实贴图时用 factor × texture）
        float* row9 = &matData[n * 36 + i * 4];
        row9[0] = obj.baseColorFactor.x;
        row9[1] = obj.baseColorFactor.y;
        row9[2] = obj.baseColorFactor.z;
        row9[3] = obj.metallicFactor;
        float* row10 = &matData[n * 40 + i * 4];
        row10[0] = obj.roughnessFactor;
        row10[1] = row10[2] = row10[3] = 0.0f;

        // 读取顶点/索引缓冲 → 每三角形 3 条顶点法线 + 3 条 UV
        auto* vb = me ? me->vertexBuffer : nullptr;
        auto* ib = me ? me->indexBuffer : nullptr;
        if (!vb || !ib || triCount == 0) { triFlat += triCount; continue; }
        const u8* vdata = static_cast<const u8*>(vb->Map());
        const u32* idata = static_cast<const u32*>(ib->Map());
        if (!vdata || !idata) {
            if (vdata) vb->Unmap();
            if (idata) ib->Unmap();
            triFlat += triCount;
            continue;
        }
        for (u32 t = 0; t < triCount; ++t) {
            for (u32 v = 0; v < 3; ++v) {
                u32 vidx = idata[t * 3 + v];
                const float* vn = reinterpret_cast<const float*>(vdata + vidx * kStride + kNormOff);
                u64 lin = (triFlat + t) * 3 + v;   // 顶点属性线性索引
                float* dst = &normalData[lin * 4];
                dst[0] = vn[0];
                dst[1] = vn[1];
                dst[2] = vn[2];
                dst[3] = 0.0f;

                const float* vuv = reinterpret_cast<const float*>(vdata + vidx * kStride + kUVOff);
                float* udst = &uvData[lin * 2];
                udst[0] = vuv[0];
                udst[1] = vuv[1];
            }
        }
        vb->Unmap();
        ib->Unmap();
        triFlat += triCount;
    }

    // ── 创建材质纹理（11×N RGBA32F）──
    {
        rhi::TextureDesc desc;
        desc.format      = rhi::Format::RGBA32_FLOAT;
        desc.width       = n;
        desc.height      = 11;
        desc.mipLevels   = 1;
        desc.usage       = rhi::TextureUsage::ShaderResource;
        desc.initialData = matData.data();
        m_SceneMaterialTex = device->CreateTexture(desc);
        if (!m_SceneMaterialTex) {
            HE_CORE_ERROR("RTPass: 场景材质纹理创建失败");
            return false;
        }
    }

    // ── 创建三角形法线纹理（width×1024 RGBA32F）──
    {
        rhi::TextureDesc desc;
        desc.format      = rhi::Format::RGBA32_FLOAT;
        desc.width       = normTexWidth;
        desc.height      = kNormTexHeight;
        desc.mipLevels   = 1;
        desc.usage       = rhi::TextureUsage::ShaderResource;
        desc.initialData = normalData.data();
        m_SceneTriangleNormals = device->CreateTexture(desc);
        if (!m_SceneTriangleNormals) {
            HE_CORE_ERROR("RTPass: 三角形法线纹理创建失败");
            return false;
        }
    }

    // ── 创建三角形 UV 纹理（width×1024 RG32F，布局与法线纹理一致）──
    {
        rhi::TextureDesc desc;
        desc.format      = rhi::Format::RG32_FLOAT;
        desc.width       = normTexWidth;
        desc.height      = kNormTexHeight;
        desc.mipLevels   = 1;
        desc.usage       = rhi::TextureUsage::ShaderResource;
        desc.initialData = uvData.data();
        m_SceneTriangleUVs = device->CreateTexture(desc);
        if (!m_SceneTriangleUVs) {
            HE_CORE_ERROR("RTPass: 三角形 UV 纹理创建失败");
            return false;
        }
    }

    HE_CORE_INFO("RTPass: 场景材质纹理(11×{}) + 法线/UV 纹理({}×{} RGBA32F/RG32F)创建, {} 实例 {} 三角形",
                 n, normTexWidth, kNormTexHeight, n, totalTris);
    return true;
}

// 创建光源 Uniform Buffer（8 盏灯 * 2 float4 + count = 272 字节）
bool RTPass::CreateLightBuffer(rhi::IRHIDevice* device, u32 maxLights) {
    if (!device) return false;
    m_LightMaxCount = std::min(maxLights, 8u);
    // colorIntensity[8] + directionType[8] + lightCount + pad
    u32 size = m_LightMaxCount * 32 + 16;  // 2 float4 per light + count
    rhi::BufferDesc ubDesc;
    ubDesc.size  = size;
    ubDesc.usage = rhi::BufferUsage::Uniform;
    m_LightUB = device->CreateBuffer(ubDesc);
    HE_CORE_INFO("RTPass: 光源 UB 创建 ({} lights, {}B)", m_LightMaxCount, size);
    return m_LightUB != nullptr;
}

// 从 GPULight SSBO 提取光源数据填充 UB（每帧调用）
void RTPass::UpdateLightBuffer(rhi::IRHIBuffer* lightBuffer) {
    if (!m_LightUB || !lightBuffer) return;

    // GPULight = 64 bytes: colorIntensity(16) + directionType(16) + positionRange(16) + coneAngles(8) + shadowIndex(4) + pad(4)
    u8* src = static_cast<u8*>(lightBuffer->Map());
    u8* dst = static_cast<u8*>(m_LightUB->Map());
    if (!src || !dst) return;

    u32 count = 0;
    for (u32 i = 0; i < m_LightMaxCount; ++i) {
        u32 srcOff = i * 64;
        float intensity = *reinterpret_cast<float*>(src + srcOff + 12); // colorIntensity.w
        if (intensity <= 0.0f) continue;  // 跳过无效光源

        // colorIntensity (float4 at offset 0)
        std::memcpy(dst + i * 32, src + srcOff, 16);
        // directionType (float4 at offset 16)
        std::memcpy(dst + i * 32 + 16, src + srcOff + 16, 16);
        count++;
    }
    // lightCount at end
    *reinterpret_cast<u32*>(dst + m_LightMaxCount * 32) = count;

    m_LightUB->Unmap();
    lightBuffer->Unmap();
}

// ============================================================
// Phase 4: 顶点拉取 — GPU 标量布局 SSBO
// ============================================================

// C++ StaticVertex 布局检测
// 不启用 GLM_FORCE_DEFAULT_ALIGNED_GENTYPES：32 字节（与 GPU 标量一致）
// 启用后 GLM vec3→16B 且 struct 对齐至 16B：48 字节（需解包）
// 【第③段第 5 批】这里原先还有 `RTVertexPacked`（GPU 标量布局 32B）与它的尺寸断言，随"顶点拉取"
// 死路径一起删除（见下）。保留 `StaticVertex` 的断言：`BuildAS` 的 `geo.vertexStride` 与
// `BuildSceneMaterialTexture` 的 `offsetof` 取法都依赖它的实际大小。
static_assert(sizeof(he::StaticVertex) == 32 || sizeof(he::StaticVertex) == 48,
              "Unexpected StaticVertex size — expected 32 or 48");

// 【第③段第 5 批】原先这里还有一条"顶点拉取"路径（`CreateVertexPullBuffer` +
// `UpdateVertexDataDescriptorSet`，把 VB/IB 打包成 32B 布局再绑到 set=1 的 binding 2/3）——
// 它**没有任何调用点**（PT 早就改用 `BuildSceneMaterialTexture` 的法线/UV 纹理查询三角形属性，
// 因为 ClosestHit 里访问 StructuredBuffer 已知 GPU fault），故连同 `RTVertexPacked`
// 与顶点拉取布局断言一起删除：留着只会让"渲染期持有组件指针"的清单多两处死代码。
// 若将来确实需要顶点拉取，正确形态是从**快照 + 网格注册表**取缓冲（本文件其它部分已改完）。

// ============================================================
// Phase 4.2: Bindless 纹理管理
// ============================================================

bool RTPass::CreateBindlessDescriptorSet(rhi::IRHIDevice* device, u32 maxTextures) {
    if (!device) return false;
    m_BindlessMaxCount = maxTextures;

    // 创建 bindless 描述符集布局
    // set=2: 纹理数组(CombinedImageSampler) + 独立采样器
    rhi::DescriptorSetLayoutDesc desc;
    desc.bindings = {
        // 【校验修复】stageMask 原为字面量 0x40 = VK_SHADER_STAGE_TASK_BIT_EXT（Task 阶段），
        // 而这一组是给 ClosestHit 采材质纹理用的（0x400）——阶段掩码写错会让校验层
        // 报 VUID-VkRayTracingPipelineCreateInfoKHR-layout-07988。改用命名常量。
        { 0, rhi::DescriptorType::CombinedImageSampler,
          maxTextures, rhi::kStageMaskClosestHit, true },  // bindless=true, ClosestHit
    };
    m_DescLayout2 = device->CreateDescriptorSetLayout(desc);
    if (m_DescLayout2 == rhi::kInvalidLayout) {
        HE_CORE_ERROR("RTPass: bindless descriptor layout 创建失败");
        return false;
    }

    // 分配描述符集
    m_DescSet2 = device->AllocateDescriptorSet(m_DescLayout2);
    if (m_DescSet2 == rhi::kInvalidSet) {
        HE_CORE_ERROR("RTPass: bindless descriptor set 分配失败");
        return false;
    }

    // 创建默认采样器（线性过滤 + 重复寻址）
    rhi::SamplerDesc samplerDesc;
    samplerDesc.minFilter = rhi::FilterMode::Linear;
    samplerDesc.magFilter = rhi::FilterMode::Linear;
    samplerDesc.mipFilter = rhi::FilterMode::Linear;
    samplerDesc.addressU  = rhi::AddressMode::Repeat;
    samplerDesc.addressV  = rhi::AddressMode::Repeat;
    samplerDesc.maxAnisotropy = 16.0f;
    m_BindlessSampler = device->CreateSampler(samplerDesc);

    // 预分配纹理/采样器数组（用默认白色纹理填充）
    m_BindlessTextures.resize(maxTextures, nullptr);
    m_BindlessSamplers.resize(maxTextures, nullptr);

    HE_CORE_INFO("RTPass: bindless 描述符集创建 (max={} textures)", maxTextures);
    return true;
}

u32 RTPass::RegisterBindlessTexture(rhi::IRHIDevice* device,
                                      rhi::IRHITexture* texture,
                                      rhi::IRHISampler* sampler) {
    if (!device || !texture || m_BindlessTextures.empty()) return ~0u;

    // 查找空闲槽位
    for (u32 i = 0; i < m_BindlessMaxCount; ++i) {
        if (m_BindlessTextures[i] == nullptr) {
            m_BindlessTextures[i] = texture;
            m_BindlessSamplers[i] = sampler ? sampler : m_BindlessSampler.get();

            // 更新描述符集（绑定此纹理）
            device->UpdateDescriptorSet(m_DescSet2, 0,
                rhi::DescriptorType::CombinedImageSampler,
                m_BindlessTextures.data(),
                m_BindlessSamplers.data(),
                m_BindlessMaxCount);
            return i;
        }
    }
    HE_CORE_WARN("RTPass: bindless 纹理数组已满 (max={})", m_BindlessMaxCount);
    return ~0u;
}

// BindDescriptorSets — 绑定所有描述符集
void RTPass::BindDescriptorSets(rhi::IRHICommandList* cmd) {
    if (m_DescSet != rhi::kInvalidSet)
        cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_DescSet);
    if (m_DescSet1 != rhi::kInvalidSet)
        cmd->BindDescriptorSet(rhi::kDescSetMaterial, m_DescSet1);
    if (m_DescSet2 != rhi::kInvalidSet)
        cmd->BindDescriptorSet(rhi::kDescSetBindless, m_DescSet2);
}

int RTPass::ReloadShader(StringView shaderName, const std::vector<u32>& newSpirv) {
    if (!m_Device || !m_Initialized) return -1;

    // 查找匹配的 shader 并替换 SPIRV
    int count = 0;
    for (auto& s : m_Shaders) {
        // shaderName 格式: "RT_Shadow.rgen" 等
        String entryName(s.entryPoint);
        if (shaderName.find("rgen") != StringView::npos && s.stage == rhi::ShaderStage::RayGen)
            { s.spirv = newSpirv; count++; }
        else if (shaderName.find("rmiss") != StringView::npos && s.stage == rhi::ShaderStage::Miss)
            { s.spirv = newSpirv; count++; }
        else if (shaderName.find("rchit") != StringView::npos && s.stage == rhi::ShaderStage::ClosestHit)
            { s.spirv = newSpirv; count++; }
        else if (shaderName.find("rcall") != StringView::npos && s.stage == rhi::ShaderStage::Callable)
            { s.spirv = newSpirv; count++; }
    }

    if (count > 0) {
        // 重建 RT PSO（保留描述符集布局和 Push Constant）
        rhi::RTPipelineStateDesc rtpDesc;
        rtpDesc.shaders        = m_Shaders;
        rtpDesc.shaderGroups   = m_ShaderGroups;
        rtpDesc.maxRecursionDepth = rhi::kRTMaxRecursionDepth;  // RayGen(0) → ClosestHit(1) → Callable(2)
        rtpDesc.maxPayloadSize    = rhi::kRTMaxPayloadSize;
        if (m_DescLayout != rhi::kInvalidLayout)
            rtpDesc.descriptorSetLayouts.push_back(m_DescLayout);
        if (m_DescLayout1 != rhi::kInvalidLayout)
            rtpDesc.descriptorSetLayouts.push_back(m_DescLayout1);
        if (m_DescLayout2 != rhi::kInvalidLayout)
            rtpDesc.descriptorSetLayouts.push_back(m_DescLayout2);
        if (m_PushConstRange.size > 0)
            rtpDesc.pushConstantRanges.push_back(m_PushConstRange);
        m_RTPipeline = m_Device->CreateRTPipelineState(rtpDesc);
        CreateSBT(m_Device);
        HE_CORE_INFO("RTPass: 热重载 {} 个 shader", count);
    }
    return count;
}

} // namespace he::render
