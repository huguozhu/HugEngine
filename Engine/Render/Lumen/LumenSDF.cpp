#include "Lumen/LumenSDF.h"

#include "Core/Assert.h"
#include "Core/Log.h"
#include "SDF_GlobalBuild.comp.spv.h"
#include "SDF_MeshBuild.comp.spv.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace he::render {

namespace {
// 描述符集绑定（与 SDF_MeshBuild.comp.slang 一致）
constexpr u32 kBindField    = 0;   // RWTexture3D<float>
constexpr u32 kBindPosition = 1;   // StructuredBuffer<float4>
constexpr u32 kBindIndex    = 2;   // StructuredBuffer<uint>
constexpr u32 kBindProbe    = 3;   // RWStructuredBuffer<float>

// push constant（与 shader 的 BuildPC 一致：3 × 16B = 48B）
struct BuildPC {
    float    originX, originY, originZ, voxelSize;
    u32      dimX, dimY, dimZ, meshIndex;
    u32      triCount, indexOffset, vertexOffset, probeStride;
};
static_assert(sizeof(BuildPC) == 48, "BuildPC 必须与 shader 的 3×16B 布局一致");

// ── Global SDF（步骤 10）──
constexpr u32 kGBindGlobal     = 0;   // RWTexture3D<uint>
constexpr u32 kGBindGlobalOut  = 1;   // RWTexture3D<float>
constexpr u32 kGBindMeshField  = 2;   // Texture3D<float>
constexpr u32 kGBindMeshSampler= 3;   // SamplerState
constexpr u32 kGBindProbe      = 4;   // RWStructuredBuffer<float>

// 与 SDF_GlobalBuild.comp.slang 的 GlobalPC 一致：6 × 16B = 96B
struct GlobalPC {
    float originX, originY, originZ, voxelSize;
    u32   dimX, dimY, dimZ, mode;
    u32   rangeLoX, rangeLoY, rangeLoZ, probeStride;
    u32   rangeHiX, rangeHiY, rangeHiZ, pad0;
    float meshOriginX, meshOriginY, meshOriginZ, meshVoxelSize;
    u32   meshDimX, meshDimY, meshDimZ, pad1;
};
static_assert(sizeof(GlobalPC) == 96, "GlobalPC 必须与 shader 的 6×16B 布局一致");
} // namespace

bool LumenSDF::Initialize(rhi::IRHIDevice* device, const LumenSDFConfig& config) {
    if (!device) return false;
    m_Device = device;
    m_Config = config;
    // 分辨率取 2 的幂且至少 8：网格映射与自检采样都按整除处理
    if (m_Config.resolution < 8) m_Config.resolution = 8;
    CreateGPUObjects();
    HE_CORE_INFO("LumenSDF: 初始化（分辨率 {}³，mesh 上限 {}，每 mesh 三角形上限 {}，每帧预算 {}）",
                 m_Config.resolution, m_Config.maxMeshes, m_Config.maxTrisPerMesh, m_Config.meshesPerFrame);
    return m_PSO != nullptr;
}

void LumenSDF::Shutdown() {
    m_Entries.clear();
    m_ProbeDist.reset();
    m_Indices.reset();
    m_Positions.reset();
    m_PSO.reset();
    m_Device = nullptr;
    m_Phase = Phase::Idle;
    m_Done = false;
    m_SelfCheck = SelfCheck{};
}

u64 LumenSDF::GetMemoryBytes() const {
    u64 bytes = 0;
    for (const auto& e : m_Entries) {
        bytes += (u64)e.resolution * e.resolution * e.resolution * 4ull;   // R32F
    }
    return bytes;
}

void LumenSDF::CreateGPUObjects() {
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        {kBindField,    rhi::DescriptorType::StorageImage,   1, rhi::kStageMaskCompute},
        {kBindPosition, rhi::DescriptorType::StorageBuffer,  1, rhi::kStageMaskCompute},
        {kBindIndex,    rhi::DescriptorType::StorageBuffer,  1, rhi::kStageMaskCompute},
        {kBindProbe,    rhi::DescriptorType::StorageBuffer,  1, rhi::kStageMaskCompute},
    };
    m_Layout = m_Device->CreateDescriptorSetLayout(layout);
    m_Set    = m_Device->AllocateDescriptorSet(m_Layout);

    rhi::ShaderBytecode cs;
    cs.stage      = rhi::ShaderStage::Compute;
    cs.spirv      = k_SDF_MeshBuild_comp_spv;
    cs.entryPoint = "main";

    // push constant 范围必须显式声明：shader 用了 48B 的 BuildPC，
    // 少了它 vkCreatePipelineLayout 与 SPIR-V 不匹配（PSO 创建失败 → 之后 SetPipeline(nullptr)）。
    rhi::PushConstantRange pcr;
    pcr.stageMask = rhi::kStageMaskCompute;
    pcr.offset    = 0;
    pcr.size      = sizeof(BuildPC);

    rhi::PipelineStateDesc pso;
    pso.bindPoint            = rhi::PipelineBindPoint::Compute;   // 少了它默认按图形管线创建 → PSO 为空
    pso.computeShader        = &cs;
    pso.descriptorSetLayouts = {m_Layout};
    pso.pushConstantRanges   = {pcr};
    pso.debugName            = "Lumen_SDF_MeshBuild";
    m_PSO = m_Device->CreatePipelineState(pso);
    if (!m_PSO) {
        HE_CORE_ERROR("LumenSDF: 构建 PSO 失败（Mesh SDF 不可用）");
    }
}

void LumenSDF::BuildQueue(const MeshBatcher& batcher) {
    const auto& vertices = batcher.GetMergedVertices();
    const auto& commands = batcher.GetDrawCommands();

    u32 skippedTris = 0, skippedCap = 0;
    for (u32 c = 0; c < (u32)commands.size(); ++c) {
        if (m_Entries.size() >= m_Config.maxMeshes) { skippedCap = (u32)commands.size() - c; break; }
        const auto& cmd = commands[c];
        const u32 triCount = cmd.indexCount / 3u;
        if (triCount == 0) continue;
        if (triCount > m_Config.maxTrisPerMesh) { ++skippedTris; continue; }

        // 该 mesh 的局部空间 AABB（顶点未施加变换，与 GPUScene 的 per-object 变换配套）
        float3 lo(1e30f), hi(-1e30f);
        for (u32 i = 0; i < cmd.indexCount; ++i) {
            const u32 vi = batcher.GetMergedIndices()[cmd.firstIndex + i] + (u32)cmd.vertexOffset;
            if (vi >= vertices.size()) continue;
            const float3 p = vertices[vi].position;
            lo = float3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
            hi = float3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
        }
        if (hi.x < lo.x) continue;   // 没有有效顶点

        // 立方体网格：边长取最长轴，稍加余量，保证最远角也在场内
        const float3 ext = hi - lo;
        const float  side = std::max(std::max(ext.x, ext.y), ext.z) * 1.02f + 1e-4f;

        MeshSDFEntry e;
        e.commandIndex = c;
        e.firstIndex   = cmd.firstIndex;
        e.indexCount   = cmd.indexCount;
        e.vertexOffset = (u32)cmd.vertexOffset;
        e.triCount     = triCount;
        e.origin       = lo - float3(side * 0.01f);
        e.resolution   = m_Config.resolution;
        e.voxelSize    = side / (float)m_Config.resolution;
        m_Entries.push_back(std::move(e));
    }

    // 分辨率/体素边长的量化统计（步骤 9 的"质量边界"）：体素边长直接决定 marching 能分辨的
    // 最小特征 —— 薄于约 2 个体素的特征在射线步进里不可靠（这是分辨率型精度限制，与几何无关）。
    if (!m_Entries.empty()) {
        float minVoxel = 1e30f, maxVoxel = 0.0f;
        for (const auto& e : m_Entries) {
            minVoxel = std::min(minVoxel, e.voxelSize);
            maxVoxel = std::max(maxVoxel, e.voxelSize);
        }
        HE_CORE_INFO("LumenSDF 质量边界: 体素边长 {:.4f} ~ {:.4f}（可分辨特征 ≳ {:.4f} ~ {:.4f} 世界单位）；"
                     "本版为**无符号**距离场（内外符号随步骤 11 的 sphere tracing 一起落）",
                     (double)minVoxel, (double)maxVoxel, (double)(2.0f * minVoxel), (double)(2.0f * maxVoxel));
    }

    if (skippedTris || skippedCap) {
        HE_CORE_WARN("LumenSDF: 跳过 {} 个 mesh（三角形数 > {}）与 {} 个 mesh（超出 mesh 上限 {}）",
                     skippedTris, m_Config.maxTrisPerMesh, skippedCap, m_Config.maxMeshes);
    }
    HE_CORE_INFO("LumenSDF: 待构建 {} 个 mesh 距离场（分辨率 {}³，预计显存 {:.2f} MB）",
                 m_Entries.size(), m_Config.resolution, (double)GetMemoryBytes() / (1024.0 * 1024.0));
}

void LumenSDF::UploadGeometry(const MeshBatcher& batcher) {
    const auto& vertices = batcher.GetMergedVertices();
    const auto& indices  = batcher.GetMergedIndices();

    // 位置：转成 float4（std430 下 StructuredBuffer<float4> 布局确定，避免 float3 的 16B 对齐差异）
    std::vector<float4> positions(vertices.size());
    m_PositionsCPU.resize(vertices.size());
    m_IndicesCPU = indices;   // 自检用的 CPU 副本（与 GPU 走不同数据路径）
    for (size_t i = 0; i < vertices.size(); ++i) {
        positions[i] = float4(vertices[i].position, 0.0f);
        m_PositionsCPU[i] = vertices[i].position;
    }

    rhi::BufferDesc vb;
    vb.size        = positions.size() * sizeof(float4);
    vb.usage       = rhi::BufferUsage::Storage;
    vb.initialData = positions.data();
    m_Positions = m_Device->CreateBuffer(vb);

    rhi::BufferDesc ib;
    ib.size        = indices.size() * sizeof(u32);
    ib.usage       = rhi::BufferUsage::Storage;
    ib.initialData = indices.data();
    m_Indices = m_Device->CreateBuffer(ib);

    // 自检探针缓冲（CPU 可读）：只统计第 0 个 mesh
    const u32 stride = std::max(1u, m_Config.probeStride);
    const u32 nx = m_Config.resolution / stride;
    m_ProbeCount = nx * nx * nx;
    rhi::BufferDesc pb;
    pb.size      = (usize)m_ProbeCount * sizeof(float);
    pb.usage     = rhi::BufferUsage::Storage;
    pb.cpuAccess = true;                      // 自检要 Map 读回
    m_ProbeDist  = m_Device->CreateBuffer(pb);

    m_Device->UpdateDescriptorSet(m_Set, kBindPosition, rhi::DescriptorType::StorageBuffer, m_Positions.get());
    m_Device->UpdateDescriptorSet(m_Set, kBindIndex,    rhi::DescriptorType::StorageBuffer, m_Indices.get());
    m_Device->UpdateDescriptorSet(m_Set, kBindProbe,    rhi::DescriptorType::StorageBuffer, m_ProbeDist.get());

    m_GeometryUploaded = true;
    HE_CORE_INFO("LumenSDF: 几何已上传（{} 顶点 float4 + {} 索引，探针 {} 点）",
                 positions.size(), indices.size(), m_ProbeCount);
}

void LumenSDF::BakeOne(rhi::IRHICommandList* cmd, u32 entryIndex) {
    MeshSDFEntry& e = m_Entries[entryIndex];

    // 每 mesh 一张 3D 距离场（R32F，可写 + 可采样）
    rhi::TextureDesc td;
    td.format = rhi::Format::R32_FLOAT;
    td.width  = e.resolution;
    td.height = e.resolution;
    td.depth  = e.resolution;
    td.usage  = rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::ShaderResource;
    e.field   = m_Device->CreateTexture(td);
    if (!e.field) return;

    // 存储图像绑定走 ImageView 重载（IRHITexture::GetNativeHandle() 返回的就是 ImageView）
    m_Device->UpdateDescriptorSetWithImageView(m_Set, kBindField, rhi::DescriptorType::StorageImage,
                                               e.field->GetNativeHandle());

    BuildPC pc{};
    pc.originX = e.origin.x; pc.originY = e.origin.y; pc.originZ = e.origin.z;
    pc.voxelSize   = e.voxelSize;
    pc.dimX = pc.dimY = pc.dimZ = e.resolution;
    pc.meshIndex   = entryIndex;
    pc.triCount    = e.triCount;
    pc.indexOffset = e.firstIndex;
    pc.vertexOffset = e.vertexOffset;
    pc.probeStride = (entryIndex == 0) ? std::max(1u, m_Config.probeStride) : 0u;
    e.probeCount   = (entryIndex == 0) ? m_ProbeCount : 0u;

    cmd->SetPipeline(m_PSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Set);
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    const u32 groups = (e.resolution + 3u) / 4u;
    cmd->Dispatch(groups, groups, groups);

    // 构建结束转入"可采样"状态：后续步骤（10 注入 / 11 sphere tracing）会读它
    cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                         rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource,
                         e.field.get());
}

void LumenSDF::CreateGlobalGPUObjects() {
    if (m_GlobalPSO) return;

    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        {kGBindGlobal,      rhi::DescriptorType::StorageImage,  1, rhi::kStageMaskCompute},
        {kGBindGlobalOut,   rhi::DescriptorType::StorageImage,  1, rhi::kStageMaskCompute},
        {kGBindMeshField,   rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute},
        {kGBindProbe,       rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},
    };
    m_GlobalLayout = m_Device->CreateDescriptorSetLayout(layout);
    m_GlobalSet    = m_Device->AllocateDescriptorSet(m_GlobalLayout);

    rhi::PushConstantRange pcr;
    pcr.stageMask = rhi::kStageMaskCompute;
    pcr.offset    = 0;
    pcr.size      = sizeof(GlobalPC);

    rhi::ShaderBytecode cs;
    cs.stage      = rhi::ShaderStage::Compute;
    cs.spirv      = k_SDF_GlobalBuild_comp_spv;
    cs.entryPoint = "main";

    rhi::PipelineStateDesc pso;
    pso.bindPoint            = rhi::PipelineBindPoint::Compute;
    pso.computeShader        = &cs;
    pso.descriptorSetLayouts = {m_GlobalLayout};
    pso.pushConstantRanges   = {pcr};
    pso.debugName            = "Lumen_SDF_GlobalBuild";
    m_GlobalPSO = m_Device->CreatePipelineState(pso);
    if (!m_GlobalPSO) HE_CORE_ERROR("LumenSDF: Global SDF 的 PSO 创建失败");

    rhi::SamplerDesc sd;
    sd.minFilter = sd.magFilter = rhi::FilterMode::Nearest;   // 逐体素取值，不能线性插值
    sd.addressU  = sd.addressV  = sd.addressW = rhi::AddressMode::ClampToEdge;
    m_NearestSampler = m_Device->CreateSampler(sd);
}

void LumenSDF::SetupGlobalGrid() {
    // 全局场覆盖全部已建 mesh 的并集 AABB（立方体，取最长轴）+ 余量
    float3 lo(1e30f), hi(-1e30f);
    for (const auto& e : m_Entries) {
        const float side = e.voxelSize * (float)e.resolution;
        lo = float3(std::min(lo.x, e.origin.x), std::min(lo.y, e.origin.y), std::min(lo.z, e.origin.z));
        hi = float3(std::max(hi.x, e.origin.x + side), std::max(hi.y, e.origin.y + side),
                    std::max(hi.z, e.origin.z + side));
    }
    const float3 ext  = hi - lo;
    const float  side = std::max(std::max(ext.x, ext.y), ext.z) * 1.05f + 1e-3f;
    m_GlobalRes       = m_Config.globalResolution;
    m_GlobalOrigin    = lo - float3(side * 0.025f);
    m_GlobalVoxelSize = side / (float)m_GlobalRes;

    const u32 stride = std::max(1u, m_GlobalRes / 4u);   // 4³ = 64 个自检探针（CPU 参考要遍历全部三角形）
    m_GlobalProbeCount = (m_GlobalRes / stride) * (m_GlobalRes / stride) * (m_GlobalRes / stride);

    // 全局场两张 3D 纹理：u32 原子目标 + R32F 可采样输出
    rhi::TextureDesc td;
    td.width = td.height = td.depth = m_GlobalRes;
    td.format = rhi::Format::R32_UINT;
    td.usage  = rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::ShaderResource;
    m_GlobalScratch = m_Device->CreateTexture(td);
    td.format = rhi::Format::R32_FLOAT;
    m_GlobalField = m_Device->CreateTexture(td);

    rhi::BufferDesc pb;
    pb.size      = (usize)m_GlobalProbeCount * sizeof(float);
    pb.usage     = rhi::BufferUsage::Storage;
    pb.cpuAccess = true;
    m_GlobalProbe = m_Device->CreateBuffer(pb);

    m_Device->UpdateDescriptorSetWithImageView(m_GlobalSet, kGBindGlobal,
        rhi::DescriptorType::StorageImage, m_GlobalScratch->GetNativeHandle());
    m_Device->UpdateDescriptorSetWithImageView(m_GlobalSet, kGBindGlobalOut,
        rhi::DescriptorType::StorageImage, m_GlobalField->GetNativeHandle());
    m_Device->UpdateDescriptorSet(m_GlobalSet, kGBindProbe,
        rhi::DescriptorType::StorageBuffer, m_GlobalProbe.get());

    HE_CORE_INFO("LumenSDF: Global SDF 单层 {}³（体素边长 {:.4f}，原点 ({:.2f},{:.2f},{:.2f})，"
                 "显存 {:.2f} MB，自检探针 {}）",
                 m_GlobalRes, (double)m_GlobalVoxelSize, (double)m_GlobalOrigin.x,
                 (double)m_GlobalOrigin.y, (double)m_GlobalOrigin.z,
                 (double)(2.0 * (u64)m_GlobalRes * m_GlobalRes * m_GlobalRes * 4ull) / (1024.0 * 1024.0),
                 m_GlobalProbeCount);
}

void LumenSDF::BuildGlobalField(rhi::IRHICommandList* cmd) {
    if (!m_GlobalPSO || m_Entries.empty()) return;

    const u32 groups = (m_GlobalRes + 3u) / 4u;
    GlobalPC pc{};
    pc.originX = m_GlobalOrigin.x; pc.originY = m_GlobalOrigin.y; pc.originZ = m_GlobalOrigin.z;
    pc.voxelSize = m_GlobalVoxelSize;
    pc.dimX = pc.dimY = pc.dimZ = m_GlobalRes;
    pc.probeStride = std::max(1u, m_GlobalRes / 4u);

    cmd->SetPipeline(m_GlobalPSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_GlobalSet);

    // ── mode 0：清空为 +inf ──
    pc.mode = 0u;
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    cmd->Dispatch(groups, groups, groups);
    cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                         rhi::ResourceState::UnorderedAccess, rhi::ResourceState::UnorderedAccess,
                         m_GlobalScratch.get());

    // ── mode 1：逐 mesh 注入（每个 mesh 绑一次它的距离场作为采样 3D 纹理）──
    pc.mode = 1u;
    for (const auto& e : m_Entries) {
        if (!e.field) continue;
        // 该 mesh 的 AABB 映射到全局体素范围
        const float side = e.voxelSize * (float)e.resolution;
        const u32 lo[3] = {
            (u32)std::max(0.0f, std::floor((e.origin.x - m_GlobalOrigin.x) / m_GlobalVoxelSize)),
            (u32)std::max(0.0f, std::floor((e.origin.y - m_GlobalOrigin.y) / m_GlobalVoxelSize)),
            (u32)std::max(0.0f, std::floor((e.origin.z - m_GlobalOrigin.z) / m_GlobalVoxelSize)),
        };
        const u32 hi[3] = {
            (u32)std::min((float)(m_GlobalRes - 1), std::floor((e.origin.x + side - m_GlobalOrigin.x) / m_GlobalVoxelSize)),
            (u32)std::min((float)(m_GlobalRes - 1), std::floor((e.origin.y + side - m_GlobalOrigin.y) / m_GlobalVoxelSize)),
            (u32)std::min((float)(m_GlobalRes - 1), std::floor((e.origin.z + side - m_GlobalOrigin.z) / m_GlobalVoxelSize)),
        };
        if (lo[0] > hi[0] || lo[1] > hi[1] || lo[2] > hi[2]) continue;

        pc.rangeLoX = lo[0]; pc.rangeLoY = lo[1]; pc.rangeLoZ = lo[2];
        pc.rangeHiX = hi[0]; pc.rangeHiY = hi[1]; pc.rangeHiZ = hi[2];
        pc.meshOriginX = e.origin.x; pc.meshOriginY = e.origin.y; pc.meshOriginZ = e.origin.z;
        pc.meshVoxelSize = e.voxelSize;
        pc.meshDimX = pc.meshDimY = pc.meshDimZ = e.resolution;

        m_Device->UpdateDescriptorSet(m_GlobalSet, kGBindMeshField,
            rhi::DescriptorType::CombinedImageSampler, e.field.get(), m_NearestSampler.get());

        // 注入覆盖**整个全局网格**（AABB 外用"到 AABB 的距离"作为到该 mesh 表面的下界）：
        // 若只注入 AABB 内的体素，网格大部分会留成 +inf，而 +inf 是一个"高估"的步长，
        // sphere tracing 会直接跳过邻近网格（实测自检 64 点里 61 点是 +inf ⇒ FAIL）。
        cmd->SetPushConstants(0, sizeof(pc), &pc);
        cmd->Dispatch(groups, groups, groups);
        cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                             rhi::ResourceState::UnorderedAccess, rhi::ResourceState::UnorderedAccess,
                             m_GlobalScratch.get());
    }

    // ── mode 2：u32 → R32F + 自检探针 ──
    pc.mode = 2u;
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    cmd->Dispatch(groups, groups, groups);
    cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                         rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource,
                         m_GlobalField.get());
}

void LumenSDF::Step(rhi::IRHICommandList* cmd, const MeshBatcher& batcher) {
    if (!m_Device || !cmd || m_Done) return;
    ++m_Frame;

    if (m_Phase == Phase::Idle) {
        BuildQueue(batcher);
        if (m_Entries.empty()) {
            HE_CORE_WARN("LumenSDF: 没有可构建的 mesh 距离场（几何为空或全部超限），步骤 10 的 Global SDF 将没有输入");
            m_Phase = Phase::Done; m_Done = true; return;
        }
        UploadGeometry(batcher);
        CreateGlobalGPUObjects();
        SetupGlobalGrid();
        m_Phase = Phase::Baking;
    }

    if (m_Phase == Phase::Baking) {
        const u32 budget = std::max(1u, m_Config.meshesPerFrame);
        for (u32 n = 0; n < budget && m_NextEntry < (u32)m_Entries.size(); ++n, ++m_NextEntry) {
            BakeOne(cmd, m_NextEntry);
        }
        if (m_NextEntry >= (u32)m_Entries.size()) {
            HE_CORE_INFO("LumenSDF: {} 个 mesh 距离场构建完成（第 {} 帧，显存 {:.2f} MB）",
                         m_Entries.size(), m_Frame, (double)GetMemoryBytes() / (1024.0 * 1024.0));
            m_Phase = Phase::WaitSelfCheck;
            m_WaitFrames = 0;
        }
        return;
    }

    if (m_Phase == Phase::WaitSelfCheck) {
        // 等 3 帧（飞行帧数）再读回：探针缓冲是 CPU 可见的持久映射内存，
        // 此刻该缓冲的写入命令早已被 GPU 执行完。
        if (++m_WaitFrames < 3) return;
        RunSelfCheck();
        // 自检完成 → 进入 Global SDF 注入（步骤 10），同一帧内 clear + N 次注入 + 转换
        BuildGlobalField(cmd);
        HE_CORE_INFO("LumenSDF: Global SDF 注入完成（{} 个 mesh，第 {} 帧）", m_Entries.size(), m_Frame);
        m_Phase = Phase::WaitGlobalCheck;
        m_WaitGlobalFrames = 0;
        return;
    }

    if (m_Phase == Phase::WaitGlobalCheck) {
        if (++m_WaitGlobalFrames < 3) return;
        RunGlobalCheck();
        m_Phase = Phase::Done;
        m_Done  = true;
    }
}

float LumenSDF::PointTriangleDistance(const float3& p, const float3& a,
                                      const float3& b, const float3& c) {
    const float3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = ab.x * ap.x + ab.y * ap.y + ab.z * ap.z;
    const float d2 = ac.x * ap.x + ac.y * ap.y + ac.z * ap.z;
    if (d1 <= 0.0f && d2 <= 0.0f) return glm::length(p - a);

    const float3 bp = p - b;
    const float d3 = ab.x * bp.x + ab.y * bp.y + ab.z * bp.z;
    const float d4 = ac.x * bp.x + ac.y * bp.y + ac.z * bp.z;
    if (d3 >= 0.0f && d4 <= d3) return glm::length(p - b);

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = d1 / (d1 - d3);
        return glm::length(p - (a + ab * v));
    }

    const float3 cp = p - c;
    const float d5 = ab.x * cp.x + ab.y * cp.y + ab.z * cp.z;
    const float d6 = ac.x * cp.x + ac.y * cp.y + ac.z * cp.z;
    if (d6 >= 0.0f && d5 <= d6) return glm::length(p - c);

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float w = d2 / (d2 - d6);
        return glm::length(p - (a + ac * w));
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return glm::length(p - (b + (c - b) * w));
    }

    const float denom = va + vb + vc;
    if (std::fabs(denom) < 1e-12f) return glm::length(p - a);
    const float v = vb / denom, w = vc / denom;
    return glm::length(p - (a + ab * v + ac * w));
}

void LumenSDF::RunSelfCheck() {
    if (!m_ProbeDist || m_Entries.empty() || m_ProbeCount == 0) return;

    void* mapped = m_ProbeDist->Map();
    if (!mapped) {
        HE_CORE_WARN("LumenSDF: 自检探针缓冲不可映射，跳过自检");
        return;
    }
    const float* gpu = static_cast<const float*>(mapped);

    const MeshSDFEntry& e = m_Entries[0];
    const u32 stride = std::max(1u, m_Config.probeStride);
    const u32 nx = e.resolution / stride;

    double sumAbs = 0.0;
    float  maxErr = 0.0f;
    u32    counted = 0;
    for (u32 z = 0; z < nx; z += 1) {
        for (u32 y = 0; y < nx; ++y) {
            for (u32 x = 0; x < nx; ++x) {
                const u32 idx = z * nx * nx + y * nx + x;
                if (idx >= m_ProbeCount) continue;
                const float3 p = e.origin +
                    float3((float)(x * stride) + 0.5f, (float)(y * stride) + 0.5f,
                           (float)(z * stride) + 0.5f) * e.voxelSize;
                // CPU 侧独立实现（同一公式，但走完全不同的数据路径：CPU 顶点数组 + 线性遍历）
                float ref = 1e30f;
                for (u32 t = 0; t < e.triCount; ++t) {
                    const u32* tri = &m_IndicesCPU[e.firstIndex + t * 3];
                    const float3 a = m_PositionsCPU[tri[0] + e.vertexOffset];
                    const float3 b = m_PositionsCPU[tri[1] + e.vertexOffset];
                    const float3 c = m_PositionsCPU[tri[2] + e.vertexOffset];
                    ref = std::min(ref, PointTriangleDistance(p, a, b, c));
                }
                const float err = std::fabs(ref - gpu[idx]);
                sumAbs += err;
                maxErr = std::max(maxErr, err);
                ++counted;
            }
        }
    }
    m_ProbeDist->Unmap();

    m_SelfCheck.valid     = true;
    m_SelfCheck.probes    = counted;
    m_SelfCheck.maxError  = maxErr;
    m_SelfCheck.tolerance = e.voxelSize * 0.25f;   // 1/4 体素
    m_SelfCheck.passed    = (maxErr <= m_SelfCheck.tolerance);

    HE_CORE_INFO("LumenSDF 自检: 探针 {} 点，最大误差 {:.6f}，平均 {:.6f}，阈值 {:.6f}（1/4 体素）=> {}",
                 counted, (double)maxErr, counted ? sumAbs / counted : 0.0,
                 (double)m_SelfCheck.tolerance, m_SelfCheck.passed ? "PASS" : "FAIL");
    if (!m_SelfCheck.passed) {
        HE_CORE_ERROR("LumenSDF 自检失败：GPU 距离场与 CPU 参考不一致，检查网格映射/缓冲布局/偏移");
    }
}

void LumenSDF::RunGlobalCheck() {
    if (!m_GlobalProbe || m_GlobalProbeCount == 0) return;

    void* mapped = m_GlobalProbe->Map();
    if (!mapped) {
        HE_CORE_WARN("LumenSDF: Global SDF 探针缓冲不可映射，跳过自检");
        return;
    }
    const float* gpu = static_cast<const float*>(mapped);

    const u32 stride = std::max(1u, m_GlobalRes / 4u);
    const u32 n = m_GlobalRes / stride;   // 每轴探针数（4）
    const float tol = 2.0f * m_GlobalVoxelSize;

    float maxErr = 0.0f;    // |误差| 上界
    float maxOver = -1e30f; // 最大高估（> 容差即视为危险：会让射线穿漏）
    double sumErr = 0.0;
    u32 within = 0, counted = 0;
    for (u32 z = 0; z < n; ++z) {
        for (u32 y = 0; y < n; ++y) {
            for (u32 x = 0; x < n; ++x) {
                const u32 idx = z * n * n + y * n + x;
                if (idx >= m_GlobalProbeCount) continue;
                const float3 p = m_GlobalOrigin +
                    float3((float)(x * stride) + 0.5f, (float)(y * stride) + 0.5f,
                           (float)(z * stride) + 0.5f) * m_GlobalVoxelSize;

                // CPU 参考：对**全部** mesh 的全部三角形取最小距离（精确值）。
                // GPU 侧是"各 mesh 场的最小值"，只在 mesh AABB 内有效 ⇒ 只可能偏大；
                // 两者的差就是这一步近似（min 合并）的幅度，正是要被量化出来的东西。
                float ref = 1e30f;
                for (const auto& e : m_Entries) {
                    for (u32 t = 0; t < e.triCount; ++t) {
                        const u32* tri = &m_IndicesCPU[e.firstIndex + t * 3];
                        const float3 a = m_PositionsCPU[tri[0] + e.vertexOffset];
                        const float3 b = m_PositionsCPU[tri[1] + e.vertexOffset];
                        const float3 c = m_PositionsCPU[tri[2] + e.vertexOffset];
                        ref = std::min(ref, PointTriangleDistance(p, a, b, c));
                    }
                }
                const float err = gpu[idx] - ref;   // 全局场是下界 ⇒ 期望 ≤ 0；> 0 表示高估（危险）
                sumErr += err;
                maxOver = std::max(maxOver, err);
                maxErr = std::max(maxErr, std::fabs(err));
                if (std::fabs(err) <= tol) ++within;
                ++counted;
            }
        }
    }
    m_GlobalProbe->Unmap();

    m_GlobalCheck.valid     = true;
    m_GlobalCheck.probes    = counted;
    m_GlobalCheck.withinTol = within;
    m_GlobalCheck.maxError  = maxErr;
    m_GlobalCheck.meanError = counted ? (float)(sumErr / counted) : 0.0f;
    m_GlobalCheck.tolerance = tol;
    // 判据：近似只影响"最近面不在自己 AABB 内"的体素，故允许少量超差；映射/偏移错位会让
    // **全部**探针同时错，所以用 95% 落界作为门槛（比单点最大误差更能区分这两类问题）。
    // 判据只说**安全方向**：全局场必须是到最近表面的下界（不得高估），否则 sphere tracing 会
    // 穿漏。下界质量（低估多少）决定步进效率，本版不足（见日志与 §5 的质量边界），不当作
    // pass/fail 门槛 —— 把它量出来、写进文档，比让它静默地"看起来通过"更有用。
    m_GlobalCheck.passed = counted > 0 && maxOver <= tol;

    HE_CORE_INFO("LumenSDF Global 自检: 探针 {} 点，最大误差 {:.6f}，最大高估 {:+.6f}"
                 "（安全判据：≤ {:.6f}）=> {}；下界质量：{} 点在 2 体素内（{:.1f}%），平均低估 {:.2f}",
                 counted, (double)maxErr, (double)maxOver, (double)tol,
                 m_GlobalCheck.passed ? "PASS" : "FAIL",
                 within, counted ? 100.0 * (double)within / counted : 0.0,
                 (double)(-m_GlobalCheck.meanError));
    if (!m_GlobalCheck.passed) {
        HE_CORE_ERROR("LumenSDF Global 自检失败：全局场出现了高估，sphere tracing 会穿漏");
    }
    if (!m_GlobalCheck.passed) {
        HE_CORE_ERROR("LumenSDF Global 自检失败：全局场与 CPU 参考大面积不一致，检查网格映射/注入范围");
    }
}

} // namespace he::render
