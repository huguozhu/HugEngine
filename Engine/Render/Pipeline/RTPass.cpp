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
                         const std::vector<rhi::RTShaderGroup>& shaderGroups) {
    m_Device = device;
    m_Shaders = rtShaders;
    m_ShaderGroups = shaderGroups;

    // 检查设备 RT 支持
    auto caps = device->GetCaps();
    if (!caps.supportsRayTracing) {
        HE_CORE_WARN("RTPass: 设备不支持 Ray Tracing，初始化跳过");
        return false;
    }

    // 1. 创建 RT Pipeline State
    rhi::RTPipelineStateDesc rtpDesc;
    rtpDesc.shaders        = rtShaders;
    rtpDesc.shaderGroups   = shaderGroups;
    rtpDesc.maxRecursionDepth = 1;
    rtpDesc.maxPayloadSize    = 4;  // 简单的 0/1 payload（可见/遮挡）
    rtpDesc.debugName      = "RTPass";

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

    // 计算 SBT 布局（假设简单映射：group[0]=RayGen, group[1]=Miss, group[2]=Hit）
    u32 align = device->GetCaps().shaderGroupBaseAlignment;
    auto aligned = [align](u32 size) -> u32 {
        return (size + align - 1) & ~(align - 1);
    };

    u32 raygenSize   = aligned(handleSize);
    u32 missSize     = aligned(handleSize);
    u32 hitGroupSize = aligned(handleSize * (groupCount > 2 ? (groupCount - 2) : 0));

    u32 totalSize = raygenSize + missSize + hitGroupSize;
    if (totalSize == 0) return false;

    // 创建 SBT 缓冲
    rhi::BufferDesc sbtDesc;
    sbtDesc.size  = totalSize;
    sbtDesc.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::AccelerationStruct
                  | rhi::BufferUsage::Uniform;
    m_SBTBuffer = device->CreateBuffer(sbtDesc);

    // 填充 SBT 句柄数据
    u8* mapped = static_cast<u8*>(m_SBTBuffer->Map());
    if (!mapped) return false;

    // RayGen (group 0)
    if (groupCount > 0) std::memcpy(mapped, handles.data(), handleSize);

    // Miss (group 1)
    if (groupCount > 1) std::memcpy(mapped + raygenSize,
                                     handles.data() + handleSize, handleSize);

    // Hit groups (groups 2+)
    for (u32 g = 2; g < groupCount; ++g) {
        std::memcpy(mapped + raygenSize + missSize + (g - 2) * hitGroupSize,
                    handles.data() + g * handleSize, handleSize);
    }
    m_SBTBuffer->Unmap();

    // 设置 SBT 描述
    m_SBT.buffer = m_SBTBuffer.get();
    if (groupCount > 0) {
        m_SBT.rayGen.handleOffset = 0;
        m_SBT.rayGen.stride       = raygenSize;
    }
    if (groupCount > 1) {
        m_SBT.miss.handleOffset = raygenSize;
        m_SBT.miss.stride       = missSize;
    }
    if (groupCount > 2) {
        m_SBT.hit.handleOffset  = raygenSize + missSize;
        m_SBT.hit.stride        = hitGroupSize;
    }

    HE_CORE_INFO("RTPass: SBT 创建完成 ({} groups, {}B total)", groupCount, totalSize);
    return true;
}

void RTPass::Shutdown() {
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
    }

    if (count > 0) {
        // 重建 RT PSO
        rhi::RTPipelineStateDesc rtpDesc;
        rtpDesc.shaders        = m_Shaders;
        rtpDesc.shaderGroups   = m_ShaderGroups;
        rtpDesc.maxRecursionDepth = 1;
        rtpDesc.maxPayloadSize    = 4;
        m_RTPipeline = m_Device->CreateRTPipelineState(rtpDesc);
        CreateSBT(m_Device);
        HE_CORE_INFO("RTPass: 热重载 {} 个 shader", count);
    }
    return count;
}

} // namespace he::render
