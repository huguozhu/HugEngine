// ============================================================
// RTPass.cpp — Ray Tracing Pass 管理器实现
// 负责 BLAS/TLAS 构建、RT PSO 管理、TraceRays 调度
// ============================================================
#include "Pipeline/RTPass.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Core/Log.h"
#include "Core/Assert.h"

#include <glm/gtc/matrix_transform.hpp>
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

    // 检查 Shader 是否有效
    if (rtShaders.empty()) {
        HE_CORE_WARN("RTPass: 未提供 RT Shader，初始化跳过");
        return false;
    }

    // 0. 分配描述符集
    if (m_DescLayout != rhi::kInvalidLayout) {
        m_DescSet = device->AllocateDescriptorSet(m_DescLayout);
    }
    if (m_DescLayout1 != rhi::kInvalidLayout) {
        m_DescSet1 = device->AllocateDescriptorSet(m_DescLayout1);
    }

    // 1. 创建 RT Pipeline State
    rhi::RTPipelineStateDesc rtpDesc;
    rtpDesc.shaders        = rtShaders;
    rtpDesc.shaderGroups   = shaderGroups;
    rtpDesc.maxRecursionDepth = 2;  // RayGen(0) → ClosestHit(1) → Callable(2)
    rtpDesc.maxPayloadSize    = 16;
    rtpDesc.maxHitAttributeSize = 8;
    rtpDesc.debugName      = "RTPass";
    for (auto& l : descLayouts) {
        if (l != rhi::kInvalidLayout)
            rtpDesc.descriptorSetLayouts.push_back(l);
    }
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

bool RTPass::CreateSBT(rhi::IRHIDevice* device) {
    if (!m_RTPipeline) return false;

    u32 groupCount   = m_RTPipeline->GetShaderGroupCount();
    u32 handleSize   = m_RTPipeline->GetShaderGroupHandleSize();
    auto handles     = m_RTPipeline->GetShaderGroupHandles();

    u32 align = device->GetCaps().shaderGroupBaseAlignment;
    auto aligned = [align](u32 size) -> u32 {
        return (size + align - 1) & ~(align - 1);
    };

    // ── 分类统计各组类型 ──
    std::vector<u32> rgIdx, missIdx, hitIdx, callIdx;
    for (u32 g = 0; g < groupCount; ++g) {
        auto t = m_ShaderGroups[g].type;
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
    sbtDesc.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::AccelerationStruct
                  | rhi::BufferUsage::Uniform;
    m_SBTBuffer = device->CreateBuffer(sbtDesc);

    u8* mapped = static_cast<u8*>(m_SBTBuffer->Map());
    if (!mapped) return false;

    for (u32 i = 0; i < rgIdx.size(); ++i)
        std::memcpy(mapped + rgOff + i * stride, handles.data() + rgIdx[i] * handleSize, handleSize);
    for (u32 i = 0; i < missIdx.size(); ++i)
        std::memcpy(mapped + msOff + i * stride, handles.data() + missIdx[i] * handleSize, handleSize);
    for (u32 i = 0; i < hitIdx.size(); ++i)
        std::memcpy(mapped + htOff + i * stride, handles.data() + hitIdx[i] * handleSize, handleSize);
    for (u32 i = 0; i < callIdx.size(); ++i)
        std::memcpy(mapped + caOff + i * stride, handles.data() + callIdx[i] * handleSize, handleSize);
    m_SBTBuffer->Unmap();

    m_SBT.buffer = m_SBTBuffer.get();
    m_SBT.rayGen.handleOffset  = rgOff;
    m_SBT.rayGen.stride        = rgIdx.empty()  ? 0 : stride;
    m_SBT.miss.handleOffset    = msOff;
    m_SBT.miss.stride          = missIdx.empty()? 0 : stride;
    m_SBT.hit.handleOffset     = htOff;
    m_SBT.hit.stride           = hitIdx.empty() ? 0 : stride;
    m_SBT.callable.handleOffset = caOff;
    m_SBT.callable.stride       = callIdx.empty()? 0 : stride;

    HE_CORE_INFO("RTPass: SBT 创建完成 ({} groups: {}RG {}Miss {}Hit {}Call, {}B)",
                 groupCount, rgIdx.size(), missIdx.size(), hitIdx.size(), callIdx.size(), total);
    return true;
}

void RTPass::Shutdown() {
    m_MaterialTex.reset();
    m_LightUB.reset();
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

static u64 HashGeometry(he::MeshComponent* mesh) {
    // 简单 hash：顶点数 + 索引数 + 缓冲地址
    u64 h = mesh->GetVertexCount();
    h = h * 31 + mesh->GetIndexCount();
    if (mesh->GetVertexBuffer())
        h = h * 31 + mesh->GetVertexBuffer()->GetDeviceAddress();
    if (mesh->GetIndexBuffer())
        h = h * 31 + mesh->GetIndexBuffer()->GetDeviceAddress();
    return h;
}

bool RTPass::HasGeometryChanged(he::MeshComponent* mesh) {
    auto it = m_BLASMap.find(mesh);
    if (it == m_BLASMap.end()) return true;
    return it->second.geometryHash != HashGeometry(mesh);
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

void RTPass::BuildAS(rhi::IRHICommandList* cmd,
                      he::World& world, he::SceneGraph& sg) {
    if (!m_Initialized) return;

    // ── 阶段 1: 构建/更新 BLAS ──
    u32 blasIdx = 0;
    world.ForEach<he::MeshComponent>([&](he::Entity entity, he::MeshComponent& mesh) {
        (void)entity;
        // 跳过无索引的空 mesh
        if (mesh.GetIndexCount() == 0) return;
        auto* vb = mesh.GetVertexBuffer().get();
        auto* ib = mesh.GetIndexBuffer().get();
        if (!vb || !ib) return;

        bool needsBuild = HasGeometryChanged(&mesh);
        if (!needsBuild) return;  // 几何未变，跳过 BLAS 重建

        // 创建或复用 BLAS entry
        auto& entry = m_BLASMap[&mesh];

        rhi::BLASBuildDesc blasDesc;
        blasDesc.flags = rhi::ASBuildFlags::PreferFastTrace;
        rhi::RTGeometryDesc geo;
        geo.type         = rhi::RTGeometryType::Triangles;
        geo.vertexBuffer = vb;
        geo.vertexFormat = rhi::Format::RGB32_FLOAT;
        geo.vertexStride = sizeof(he::StaticVertex);
        geo.maxVertex    = mesh.GetVertexCount();
        geo.indexBuffer  = ib;
        geo.indexFormat  = rhi::Format::R32_UINT;
        geo.maxPrimitiveCount = mesh.GetIndexCount() / 3;
        blasDesc.geometries.push_back(geo);

        // 查询构建所需大小
        rhi::ASBuildSizes sizes = m_Device->GetBLASBuildSizes(blasDesc);

        // 创建 BLAS（或复用已有 + resize）
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

        // 构建 BLAS
        cmd->BuildBLAS(entry.blas.get(), entry.scratchBuffer.get(), blasDesc, false);

        // 记录 hash
        entry.geometryHash = HashGeometry(&mesh);
        HE_CORE_INFO("RTPass: BLAS 构建 (mesh#{}, vertices={}, triangles={}, size={}KB)",
                     blasIdx++, mesh.GetVertexCount(),
                     mesh.GetIndexCount() / 3,
                     sizes.accelerationStructureSize / 1024);
    });

    // ── 阶段 2: 构建 TLAS（每帧）──
    std::vector<rhi::TLASInstanceDesc> instances;
    u32 instanceID = 0;

    world.ForEach<he::MeshComponent>([&](he::Entity entity, he::MeshComponent& mesh) {
        if (mesh.GetIndexCount() == 0) return;
        auto it = m_BLASMap.find(&mesh);
        if (it == m_BLASMap.end()) return;

        // 获取世界变换矩阵（通过 SceneGraph）
        float4x4 worldMatrix = sg.GetWorldMatrix(entity);

        rhi::TLASInstanceDesc inst;
        inst.transform   = ToTransformMatrix(worldMatrix);
        inst.instanceID  = instanceID++;      // 实例自定义 ID
        inst.instanceMask = 0xFF;             // 所有光线可见
        inst.sbtOffset   = 0;                // 命中组索引（简单场景：0）
        inst.flags       = 0;                // VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR
        inst.blasAddress = it->second.blas->GetDeviceAddress();
        instances.push_back(inst);
    });

    if (!instances.empty()) {
        // 上传实例数据到 GPU
        void* mapped = m_TLASInstanceBuffer->Map();
        std::memcpy(mapped, instances.data(),
                     instances.size() * sizeof(rhi::TLASInstanceDesc));
        m_TLASInstanceBuffer->Unmap();

        // 构建 TLAS
        cmd->BuildTLAS(m_TLAS.get(), m_TLASScratch.get(),
                       m_TLASInstanceBuffer.get(),
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
                                    he::World& world) {
    if (!device || maxInstances == 0) return false;
    m_MaterialInstanceCount = std::min(maxInstances, 256u);

    // 3 行 × N 列，默认白色兜底
    std::vector<float> texData(m_MaterialInstanceCount * 4 * 3, 1.0f);

    // 从 MeshComponent 收集 PBR 材质数据（按 TLAS 实例顺序）
    u32 idx = 0;
    world.ForEach<he::MeshComponent>([&](he::Entity, he::MeshComponent& mesh) {
        if (mesh.GetIndexCount() == 0) return;
        if (idx >= m_MaterialInstanceCount) return;
        // Row 0: baseColorFactor
        float* row0 = &texData[idx * 4];
        row0[0] = mesh.baseColorFactor.x;
        row0[1] = mesh.baseColorFactor.y;
        row0[2] = mesh.baseColorFactor.z;
        row0[3] = mesh.baseColorFactor.w;
        // Row 1: metallic, roughness, ao, alphaCutoff
        float* row1 = &texData[m_MaterialInstanceCount * 4 + idx * 4];
        row1[0] = mesh.metallicFactor;
        row1[1] = mesh.roughnessFactor;
        row1[2] = mesh.aoFactor;
        row1[3] = mesh.alphaCutoff;
        // Row 2: materialID (uint bitcast to float), 0, 0, 0
        float* row2 = &texData[m_MaterialInstanceCount * 4 * 2 + idx * 4];
        std::memcpy(&row2[0], &mesh.materialID, 4);       // uint → float bitcast
        row2[1] = 0.0f;  // materialFlags 在 GPUObjectData 层计算，此处预留
        row2[2] = 0.0f;
        row2[3] = 0.0f;
        idx++;
    });

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

// BindDescriptorSets — 绑定所有描述符集
void RTPass::BindDescriptorSets(rhi::IRHICommandList* cmd) {
    if (m_DescSet != rhi::kInvalidSet)
        cmd->BindDescriptorSet(0, m_DescSet);
    if (m_DescSet1 != rhi::kInvalidSet)
        cmd->BindDescriptorSet(1, m_DescSet1);
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
        rtpDesc.maxRecursionDepth = 2;  // RayGen(0) → ClosestHit(1) → Callable(2)
        rtpDesc.maxPayloadSize    = 16;
        if (m_DescLayout != rhi::kInvalidLayout)
            rtpDesc.descriptorSetLayouts.push_back(m_DescLayout);
        if (m_DescLayout1 != rhi::kInvalidLayout)
            rtpDesc.descriptorSetLayouts.push_back(m_DescLayout1);
        if (m_PushConstRange.size > 0)
            rtpDesc.pushConstantRanges.push_back(m_PushConstRange);
        m_RTPipeline = m_Device->CreateRTPipelineState(rtpDesc);
        CreateSBT(m_Device);
        HE_CORE_INFO("RTPass: 热重载 {} 个 shader", count);
    }
    return count;
}

} // namespace he::render
