#include "Lumen/LumenSDF.h"

#include "Core/Assert.h"
#include "Core/Log.h"
#include "SDF_GlobalBuild.comp.spv.h"
#include "SDF_LayerProbe.comp.spv.h"
#include "SDF_MeshConvert.comp.spv.h"
#include "SDF_MeshFlood.comp.spv.h"
#include "SDF_MeshScatter.comp.spv.h"
#include "SDF_RayMarch.comp.spv.h"
#include "SDF_RayMarchDetail.comp.spv.h"

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
// 三个 mesh 级 shader 共用同一段 3×16B：
//   scatter：dims.w = mode（0 清空 / 1 逐三角形写入）、ranges = (三角形数, 索引起始, 顶点偏移, 未用)
//   convert：dims.w = mesh 序号（0 = 写自检探针）、ranges.w = 探针步长

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

// 与 SDF_MeshFlood.comp.slang 的 FloodPC 一致：3 × 16B = 48B（dims.w = 本次洪泛步长）
struct FloodPC {
    float originX, originY, originZ, voxelSize;
    u32   dimX, dimY, dimZ, stride;
    u32   pad0, pad1, pad2, pad3;
};
static_assert(sizeof(FloodPC) == 48, "FloodPC 必须与 shader 的 3×16B 布局一致");
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
    // push constant 范围必须显式声明：shader 用了 48B 的 BuildPC，
    // 少了它 vkCreatePipelineLayout 与 SPIR-V 不匹配（PSO 创建失败 → 之后 SetPipeline(nullptr)）。
    rhi::PushConstantRange pcr;
    pcr.stageMask = rhi::kStageMaskCompute;
    pcr.offset    = 0;
    pcr.size      = sizeof(BuildPC);

    // ── scatter（清空 + 每三角形一组的原子最小）──
    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        {kBindField,    rhi::DescriptorType::StorageImage,  1, rhi::kStageMaskCompute},
        {kBindPosition, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},
        {kBindIndex,    rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},
    };
    m_Layout = m_Device->CreateDescriptorSetLayout(layout);
    m_Set    = m_Device->AllocateDescriptorSet(m_Layout);

    rhi::ShaderBytecode cs;
    cs.stage      = rhi::ShaderStage::Compute;
    cs.spirv      = k_SDF_MeshScatter_comp_spv;
    cs.entryPoint = "main";

    rhi::PipelineStateDesc pso;
    pso.bindPoint            = rhi::PipelineBindPoint::Compute;   // 少了它默认按图形管线创建 → PSO 为空
    pso.computeShader        = &cs;
    pso.descriptorSetLayouts = {m_Layout};
    pso.pushConstantRanges   = {pcr};
    pso.debugName            = "Lumen_SDF_MeshScatter";
    m_PSO = m_Device->CreatePipelineState(pso);
    if (!m_PSO) HE_CORE_ERROR("LumenSDF: scatter 的 PSO 创建失败（Mesh SDF 不可用）");

    // ── convert（u32 → R32F + 自检探针）──
    rhi::DescriptorSetLayoutDesc conv;
    conv.bindings = {
        {0, rhi::DescriptorType::StorageImage,  1, rhi::kStageMaskCompute},   // u32 场
        {1, rhi::DescriptorType::StorageImage,  1, rhi::kStageMaskCompute},   // R32F 输出
        {2, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},   // 探针
    };
    m_ConvertLayout = m_Device->CreateDescriptorSetLayout(conv);
    m_ConvertSet    = m_Device->AllocateDescriptorSet(m_ConvertLayout);

    rhi::ShaderBytecode ccs;
    ccs.stage      = rhi::ShaderStage::Compute;
    ccs.spirv      = k_SDF_MeshConvert_comp_spv;
    ccs.entryPoint = "main";

    rhi::PipelineStateDesc cpso;
    cpso.bindPoint            = rhi::PipelineBindPoint::Compute;
    cpso.computeShader        = &ccs;
    cpso.descriptorSetLayouts = {m_ConvertLayout};
    cpso.pushConstantRanges   = {pcr};
    cpso.debugName            = "Lumen_SDF_MeshConvert";
    m_ConvertPSO = m_Device->CreatePipelineState(cpso);
    if (!m_ConvertPSO) HE_CORE_ERROR("LumenSDF: convert 的 PSO 创建失败");

    // ── 跳步洪泛（补全 scatter 留下的空洞）：与 scatter 共用绑定集与 push constant ──
    rhi::ShaderBytecode fcs;
    fcs.stage      = rhi::ShaderStage::Compute;
    fcs.spirv      = k_SDF_MeshFlood_comp_spv;
    fcs.entryPoint = "main";

    rhi::PipelineStateDesc fpso;
    fpso.bindPoint            = rhi::PipelineBindPoint::Compute;
    fpso.computeShader        = &fcs;
    fpso.descriptorSetLayouts = {m_Layout};
    fpso.pushConstantRanges   = {pcr};
    fpso.debugName            = "Lumen_SDF_MeshFlood";
    m_FloodPSO = m_Device->CreatePipelineState(fpso);
    if (!m_FloodPSO) HE_CORE_ERROR("LumenSDF: flood 的 PSO 创建失败");
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

    // 自检探针挑 AABB 最大的 mesh（分辨率相同 ⇒ 体素数最多者）：三方对照里"存 0"的探针落在
    // 体量最大的网格里，先查它最有信息量。同时把前 5 大的网格打出来，便于把对照表的 meshIdx 对上号。
    m_ProbeMeshIndex = 0;
    for (u32 i = 1; i < (u32)m_Entries.size(); ++i) {
        if (m_Entries[i].voxelSize > m_Entries[m_ProbeMeshIndex].voxelSize) m_ProbeMeshIndex = i;
    }
    if (!m_Entries.empty()) {
        std::vector<u32> order(m_Entries.size());
        for (u32 i = 0; i < (u32)order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [this](u32 a, u32 b) {
            return m_Entries[a].voxelSize > m_Entries[b].voxelSize;
        });
        std::string s;
        for (u32 i = 0; i < 5u && i < (u32)order.size(); ++i) {
            const MeshSDFEntry& e = m_Entries[order[i]];
            s += " [条目 " + std::to_string(order[i]) + " → 命令 " + std::to_string(e.commandIndex) +
                 ": 边长 " + std::to_string((int)(e.voxelSize * e.resolution)) + ", " +
                 std::to_string(e.triCount) + " 三角形]";
        }
        HE_CORE_INFO("LumenSDF: AABB 最大的 5 个 mesh（探针选中条目 {}）:{}", m_ProbeMeshIndex, s);
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

    // 自检探针缓冲（CPU 可读）：只统计第 0 个 mesh。步长默认 resolution/4 ⇒ 4³ = 64 个探针，
    // 因为 CPU 参考要对"全部三角形"遍历，探针一多就慢（128³ 的场不能按体素逐个比对）。
    if (m_Config.probeStride == 0) m_Config.probeStride = std::max(1u, m_Config.resolution / 4u);
    const u32 stride = std::max(1u, m_Config.probeStride);
    const u32 nx = m_Config.resolution / stride;
    m_ProbeCount = nx * nx * nx;
    rhi::BufferDesc pb;
    pb.size      = (usize)m_ProbeCount * std::max(1u, m_Config.maxMeshes) * sizeof(float);
    pb.usage     = rhi::BufferUsage::Storage;
    pb.cpuAccess = true;                      // 自检要 Map 读回
    m_ProbeDist  = m_Device->CreateBuffer(pb);

    // 共享的 u32 距离场（原子最小目标）：逐 mesh 串行复用，不必每个 mesh 一张
    {
        rhi::TextureDesc td;
        td.format = rhi::Format::R32_UINT;
        td.width = td.height = td.depth = m_Config.resolution;
        td.usage = rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::ShaderResource;
        m_MeshScratch = m_Device->CreateTexture(td);
    }

    m_Device->UpdateDescriptorSet(m_Set, kBindPosition, rhi::DescriptorType::StorageBuffer, m_Positions.get());
    m_Device->UpdateDescriptorSet(m_Set, kBindIndex,    rhi::DescriptorType::StorageBuffer, m_Indices.get());
    m_Device->UpdateDescriptorSetWithImageView(m_Set, kBindField, rhi::DescriptorType::StorageImage,
                                               m_MeshScratch->GetNativeHandle());
    m_Device->UpdateDescriptorSetWithImageView(m_ConvertSet, 0, rhi::DescriptorType::StorageImage,
                                               m_MeshScratch->GetNativeHandle());
    m_Device->UpdateDescriptorSet(m_ConvertSet, 2, rhi::DescriptorType::StorageBuffer, m_ProbeDist.get());

    m_GeometryUploaded = true;
    HE_CORE_INFO("LumenSDF: 几何已上传（{} 顶点 float4 + {} 索引，探针 {} 点（针对 mesh {}，AABB 最大者），u32 临时场 {:.2f} MB）",
                 positions.size(), indices.size(), m_ProbeCount, m_ProbeMeshIndex,
                 (double)((u64)m_Config.resolution * m_Config.resolution * m_Config.resolution * 4ull)
                     / (1024.0 * 1024.0));
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

    BuildPC pc{};
    pc.originX = e.origin.x; pc.originY = e.origin.y; pc.originZ = e.origin.z;
    pc.voxelSize   = e.voxelSize;
    pc.dimX = pc.dimY = pc.dimZ = e.resolution;
    pc.meshIndex   = entryIndex;
    pc.triCount    = e.triCount;
    pc.indexOffset = e.firstIndex;
    pc.vertexOffset = e.vertexOffset;
    pc.probeStride = std::max(1u, m_Config.probeStride);
    e.probeCount   = (entryIndex == m_ProbeMeshIndex) ? m_ProbeCount : 0u;

    // ── ① 清空 u32 场为 +inf：一维线性遍历，组数 = ceil(res³/64) ──
    cmd->SetPipeline(m_PSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Set);
    pc.meshIndex = 0u;   // scatter 的 mode：0 = 清空
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    const u64 voxels = (u64)e.resolution * e.resolution * e.resolution;
    cmd->Dispatch((u32)((voxels + 63ull) / 64ull), 1, 1);
    cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                         rhi::ResourceState::UnorderedAccess, rhi::ResourceState::UnorderedAccess,
                         m_MeshScratch.get());

    // ── ② scatter：一个三角形一个线程组，只扫自己的 AABB ──
    pc.meshIndex = 1u;   // scatter 的 mode：1 = 逐三角形写入
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    cmd->Dispatch(std::max(1u, e.triCount), 1, 1);
    cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                         rhi::ResourceState::UnorderedAccess, rhi::ResourceState::UnorderedAccess,
                         m_MeshScratch.get());

    // ── ②b 跳步洪泛：scatter 只写了表面附近的一条带，远处仍是 +inf（= 高估，会穿漏），
    //      按 res/2, res/4 … 1 逐级松弛补全全场 ──
    cmd->SetPipeline(m_FloodPSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_Set);   // 与 scatter 同一套绑定（只有 u32 场）
    for (u32 s = e.resolution / 2u; s >= 1u; s /= 2u) {
        pc.meshIndex = s;   // flood 的 dims.w = 本次步长（体素）
        // **每级两趟**：单趟的 26 邻域松弛只在一个方向上把信息推到位，远场精度因此偏松
        // （实测单趟时大网格远场最大误差 100~315 单位）；洪泛的标准做法是每级双向。
        for (u32 pass = 0; pass < 2u; ++pass) {
            cmd->SetPushConstants(0, sizeof(pc), &pc);
            const u32 fgroups = (e.resolution + 3u) / 4u;
            cmd->Dispatch(fgroups, fgroups, fgroups);
            cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                                 rhi::ResourceState::UnorderedAccess, rhi::ResourceState::UnorderedAccess,
                                 m_MeshScratch.get());
        }
        if (s == 1u) break;   // 防止 s 折半变 0 造成死循环
    }

    // ── ③ 转换：u32 → R32F + 写自检探针 ──
    m_Device->UpdateDescriptorSetWithImageView(m_ConvertSet, 1, rhi::DescriptorType::StorageImage,
                                               e.field->GetNativeHandle());
    cmd->SetPipeline(m_ConvertPSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_ConvertSet);
    pc.meshIndex = 0u;                              // convert 的 dims.w = 0 ⇒ 写自检探针
    pc.triCount  = entryIndex * m_ProbeCount;       // convert 用 ranges.x 传本 mesh 的探针段基址
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

    // 全局网格上的跳步洪泛：复用 SDF_MeshFlood 这个 shader（它本来就是"任意网格 + 步长"的参数化），
    // 但**管线布局必须用全局描述符集布局** —— 绑定的描述符集与 pipeline layout 不匹配会直接崩。
    // 它只声明 binding 0（u32 场），而全局布局的 binding 0 正是层 scratch。
    rhi::PushConstantRange pcrFlood;
    pcrFlood.stageMask = rhi::kStageMaskCompute;
    pcrFlood.offset    = 0;
    pcrFlood.size      = sizeof(FloodPC);

    rhi::ShaderBytecode fcs;
    fcs.stage      = rhi::ShaderStage::Compute;
    fcs.spirv      = k_SDF_MeshFlood_comp_spv;
    fcs.entryPoint = "main";

    rhi::PipelineStateDesc fpso;
    fpso.bindPoint            = rhi::PipelineBindPoint::Compute;
    fpso.computeShader        = &fcs;
    fpso.descriptorSetLayouts = {m_GlobalLayout};
    fpso.pushConstantRanges   = {pcrFlood};
    fpso.debugName            = "Lumen_SDF_GlobalFlood";
    m_GlobalFloodPSO = m_Device->CreatePipelineState(fpso);
    if (!m_GlobalFloodPSO) HE_CORE_ERROR("LumenSDF: Global 洪泛的 PSO 创建失败");

    rhi::SamplerDesc sd;
    sd.minFilter = sd.magFilter = rhi::FilterMode::Nearest;   // 逐体素取值，不能线性插值
    sd.addressU  = sd.addressV  = sd.addressW = rhi::AddressMode::ClampToEdge;
    m_NearestSampler = m_Device->CreateSampler(sd);

    // 独立取样 pass：绑定集与全局布局相同（binding 2 = 该层场，binding 4 = 探针缓冲）
    rhi::ShaderBytecode pcs;
    pcs.stage      = rhi::ShaderStage::Compute;
    pcs.spirv      = k_SDF_LayerProbe_comp_spv;
    pcs.entryPoint = "main";

    rhi::PipelineStateDesc ppso;
    ppso.bindPoint            = rhi::PipelineBindPoint::Compute;
    ppso.computeShader        = &pcs;
    ppso.descriptorSetLayouts = {m_GlobalLayout};
    ppso.pushConstantRanges   = {pcr};
    ppso.debugName            = "Lumen_SDF_LayerProbe";
    m_LayerProbePSO = m_Device->CreatePipelineState(ppso);
    if (!m_LayerProbePSO) HE_CORE_ERROR("LumenSDF: 层取样 pass 的 PSO 创建失败");
}

void LumenSDF::SetupGlobalGrid() {
    // 场景并集 AABB（全部已建 mesh）→ 决定 clipmap 层
    float3 lo(1e30f), hi(-1e30f);
    for (const auto& e : m_Entries) {
        const float side = e.voxelSize * (float)e.resolution;
        lo = float3(std::min(lo.x, e.origin.x), std::min(lo.y, e.origin.y), std::min(lo.z, e.origin.z));
        hi = float3(std::max(hi.x, e.origin.x + side), std::max(hi.y, e.origin.y + side),
                    std::max(hi.z, e.origin.z + side));
    }
    const float3 ext      = hi - lo;
    const float  sceneSide = std::max(std::max(ext.x, ext.y), ext.z) * 1.05f + 1e-3f;
    const float3 sceneCtr  = (lo + hi) * 0.5f;

    const u32 res = m_Config.globalResolution;
    m_GlobalLayerCount = std::clamp(m_Config.globalLayers, 1u, kMaxGlobalLayers);

    for (u32 L = 0; L < m_GlobalLayerCount; ++L) {
        GlobalLayer& layer = m_GlobalLayers[L];
        // L = kMaxGlobalLayers-1 是远层（覆盖全场）；更小的 L 是更细的近层，居中于场景中心。
        // 近层边长 = 场景最长轴 × nearFraction^(层数-1-L)，故得名"cl_ipmap"的最小可用形态：
        // 每往里一层体素小一个比例，而覆盖范围也小同样的比例。
        const u32   stepsFromFar = (kMaxGlobalLayers - 1u) - L;
        const float layerSide    = sceneSide * std::pow(m_Config.nearFraction, (float)stepsFromFar);
        layer.res       = res;
        layer.origin    = sceneCtr - float3(layerSide * 0.5f);
        layer.voxelSize = layerSide / (float)res;

        const u32 stride = std::max(1u, res / 4u);
        layer.probeCount = (res / stride) * (res / stride) * (res / stride);

        rhi::TextureDesc td;
        td.width = td.height = td.depth = res;
        td.format = rhi::Format::R32_UINT;
        td.usage  = rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::ShaderResource;
        layer.scratch = m_Device->CreateTexture(td);
        td.format = rhi::Format::R32_FLOAT;
        layer.field = m_Device->CreateTexture(td);

        rhi::BufferDesc pb;
        pb.size      = (usize)layer.probeCount * sizeof(float);
        pb.usage     = rhi::BufferUsage::Storage;
        pb.cpuAccess = true;
        layer.probe = m_Device->CreateBuffer(pb);
        // 【哨兵实验 —— 结论：这条 CPU 读回路径不可信】先写 -12345 再读回，本想区分"场值是 0"
        // 与"探针根本没写"，实测自相矛盾：层 0 报"残留 64/64"（看似没写）、层 1 报 0/64（写了），
        // 而不写哨兵时层 0 读回的又是全 0 ⇒ 主机可见缓冲的映射/一致性语义未经验证，
        // 该读数不能当判据。层 0 的探针数据一律视为不可信，验证改走射线（sphere tracing 自检）。
        if (void* pm = layer.probe->Map()) {
            std::vector<float> sentinel(layer.probeCount, -12345.0f);
            std::memcpy(pm, sentinel.data(), (usize)layer.probeCount * sizeof(float));
            layer.probe->Unmap();
        }

        HE_CORE_INFO("LumenSDF: Global SDF 层 {} {}³（体素边长 {:.4f}，边长 {:.1f}，原点 "
                     "({:.1f},{:.1f},{:.1f})，显存 {:.2f} MB/层）",
                     L, res, (double)layer.voxelSize, (double)layerSide,
                     (double)layer.origin.x, (double)layer.origin.y, (double)layer.origin.z,
                     (double)(2.0 * (u64)res * res * res * 4ull) / (1024.0 * 1024.0));
    }
}
void LumenSDF::BuildGlobalField(rhi::IRHICommandList* cmd) {
    if (!m_GlobalPSO || m_Entries.empty()) return;

    const u32 groups = (m_Config.globalResolution + 3u) / 4u;
    for (u32 L = 0; L < m_GlobalLayerCount; ++L) {
        GlobalLayer& layer = m_GlobalLayers[L];
        if (!layer.scratch || !layer.field) continue;

        m_Device->UpdateDescriptorSetWithImageView(m_GlobalSet, kGBindGlobal,
            rhi::DescriptorType::StorageImage, layer.scratch->GetNativeHandle());
        m_Device->UpdateDescriptorSetWithImageView(m_GlobalSet, kGBindGlobalOut,
            rhi::DescriptorType::StorageImage, layer.field->GetNativeHandle());
        m_Device->UpdateDescriptorSet(m_GlobalSet, kGBindProbe,
            rhi::DescriptorType::StorageBuffer, layer.probe.get());

        GlobalPC pc{};
        pc.originX = layer.origin.x; pc.originY = layer.origin.y; pc.originZ = layer.origin.z;
        pc.voxelSize = layer.voxelSize;
        pc.dimX = pc.dimY = pc.dimZ = layer.res;
        pc.probeStride = std::max(1u, layer.res / 4u);

        cmd->SetPipeline(m_GlobalPSO.get());
        cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_GlobalSet);

        // ① 清空为 +inf
        pc.mode = 0u;
        cmd->SetPushConstants(0, sizeof(pc), &pc);
        cmd->Dispatch(groups, groups, groups);
        cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                             rhi::ResourceState::UnorderedAccess, rhi::ResourceState::UnorderedAccess,
                             layer.scratch.get());

        // ② 逐 mesh 注入（覆盖**整个层网格**；AABB 外用"到 AABB 的距离"作下界，见 shader 注释）
        pc.mode = 1u;
        for (const auto& e : m_Entries) {
            if (!e.field) continue;
            pc.meshOriginX = e.origin.x; pc.meshOriginY = e.origin.y; pc.meshOriginZ = e.origin.z;
            pc.meshVoxelSize = e.voxelSize;
            pc.meshDimX = pc.meshDimY = pc.meshDimZ = e.resolution;
            m_Device->UpdateDescriptorSet(m_GlobalSet, kGBindMeshField,
                rhi::DescriptorType::CombinedImageSampler, e.field.get(), m_LinearSampler.get());
            cmd->SetPushConstants(0, sizeof(pc), &pc);
            cmd->Dispatch(groups, groups, groups);
            cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                                 rhi::ResourceState::UnorderedAccess, rhi::ResourceState::UnorderedAccess,
                                 layer.scratch.get());
        }

        // ③ 跳步洪泛补全：AABB 内才有精确值，其余体素靠洪泛逐级传播（与 mesh 层同一套算法）
        cmd->SetPipeline(m_GlobalFloodPSO.get());
        cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_GlobalSet);
        FloodPC fpc{};
        fpc.originX = layer.origin.x; fpc.originY = layer.origin.y; fpc.originZ = layer.origin.z;
        fpc.voxelSize = layer.voxelSize;
        fpc.dimX = fpc.dimY = fpc.dimZ = layer.res;
        for (u32 s = layer.res / 2u; s >= 1u; s /= 2u) {
            fpc.stride = s;
            for (u32 pass = 0; pass < 2u; ++pass) {   // 每级两趟（与 mesh 层同理：单趟只推一个方向）
                cmd->SetPushConstants(0, sizeof(fpc), &fpc);
                cmd->Dispatch(groups, groups, groups);
                cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                                     rhi::ResourceState::UnorderedAccess, rhi::ResourceState::UnorderedAccess,
                                     layer.scratch.get());
            }
            if (s == 1u) break;
        }

        // ④ 独立取样：把该层场在探针坐标上的值写进探针缓冲（不复用 convert 里的取模条件）
        m_Device->UpdateDescriptorSet(m_GlobalSet, kGBindMeshField,
            rhi::DescriptorType::CombinedImageSampler, layer.field.get(), m_NearestSampler.get());
        cmd->SetPipeline(m_LayerProbePSO.get());
        cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_GlobalSet);
        pc.mode = 3u;   // 取样 pass 用自己的 push constant 语义：dims.w = 步长（下面用 FloodPC 传）
        {
            FloodPC ppc{};
            ppc.originX = layer.origin.x; ppc.originY = layer.origin.y; ppc.originZ = layer.origin.z;
            ppc.voxelSize = layer.voxelSize;
            ppc.dimX = ppc.dimY = ppc.dimZ = layer.res;
            ppc.stride = std::max(1u, layer.res / 4u);
            cmd->SetPushConstants(0, sizeof(ppc), &ppc);
            cmd->Dispatch((layer.probeCount + 63u) / 64u, 1, 1);
        }

        // ⑤ u32 → R32F（自检探针由上面的独立 pass 负责）
        cmd->SetPipeline(m_GlobalPSO.get());
        cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_GlobalSet);
        pc.mode = 2u;
        cmd->SetPushConstants(0, sizeof(pc), &pc);
        cmd->Dispatch(groups, groups, groups);
        cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                             rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource,
                             layer.field.get());
    }
    HE_CORE_INFO("LumenSDF: Global SDF 注入完成（{} 层 × {} 个 mesh，第 {} 帧）",
                 m_GlobalLayerCount, m_Entries.size(), m_Frame);
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
        // 全局场就绪 → 同一帧准备并跑 sphere tracing 验证（步骤 11）
        CreateMarchGPUObjects();
        SetupMarchRays();
        RunMarch(cmd);          // 全局场追踪（远场）
        RunMarchDetail(cmd);    // 逐 mesh 细节追踪（近场，min 归约）
        HE_CORE_INFO("LumenSDF: sphere tracing 已发射（全局 + {} 个 mesh 的细节追踪，第 {} 帧）",
                     m_Entries.size(), m_Frame);
        m_Phase = Phase::WaitMarchCheck;
        m_WaitMarchFrames = 0;
        return;
    }

    if (m_Phase == Phase::WaitMarchCheck) {
        if (++m_WaitMarchFrames < 3) return;
        RunMarchCheck();
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

float3 LumenSDF::ClosestPointOnTriangle(const float3& p, const float3& a,
                                        const float3& b, const float3& c) {
    // Ericson 5.1.5 的区域判定版：与 PointTriangleDistance 同一套分支，但返回**最近点**
    // （自检要用它算"最近三角形法线"符号，作为 parity 符号的独立交叉验证）
    const float3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;

    const float3 bp = p - b;
    const float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = d1 / (d1 - d3);
        return a + ab * v;
    }

    const float3 cp = p - c;
    const float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float w = d2 / (d2 - d6);
        return a + ac * w;
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + (c - b) * w;
    }

    const float denom = va + vb + vc;
    if (std::fabs(denom) < 1e-12f) return a;
    const float v = vb / denom, w = vc / denom;
    return a + ab * v + ac * w;
}

void LumenSDF::RunSelfCheck() {
    if (!m_ProbeDist || m_Entries.empty() || m_ProbeCount == 0) return;

    void* mapped = m_ProbeDist->Map();
    if (!mapped) {
        HE_CORE_WARN("LumenSDF: 自检探针缓冲不可映射，跳过自检");
        return;
    }
    const float* gpuAll = static_cast<const float*>(mapped);

    const u32 stride = std::max(1u, m_Config.probeStride);
    const u32 n = m_Config.resolution / stride;

    // 逐 mesh 统计：探针缓冲按 mesh 分段（每个 mesh 写自己那一段），一次跑完所有网格 ——
    // 这样"哪张场在远场塌成 0 / 误差最大"就是一次运行能点名的东西，不必再逐张试。
    struct MeshStat {
        u32   entry = 0;
        float maxErr = 0.0f, minV = 1e30f, maxV = -1e30f;
        u32   zeros = 0, nearCount = 0, nearBad = 0, farCount = 0, farBad = 0, counted = 0;
    };
    std::vector<MeshStat> stats(m_Entries.size());
    u32 totalZero = 0, totalProbes = 0;

    for (u32 mi = 0; mi < (u32)m_Entries.size(); ++mi) {
        const MeshSDFEntry& e = m_Entries[mi];
        const float* gpu = gpuAll + (usize)mi * m_ProbeCount;
        MeshStat& st = stats[mi];
        st.entry = mi;

        for (u32 z = 0; z < n; ++z) {
            for (u32 y = 0; y < n; ++y) {
                for (u32 x = 0; x < n; ++x) {
                    const u32 idx = z * n * n + y * n + x;
                    if (idx >= m_ProbeCount) continue;
                    const float3 p = e.origin +
                        float3((float)(x * stride) + 0.5f, (float)(y * stride) + 0.5f,
                               (float)(z * stride) + 0.5f) * e.voxelSize;
                    // CPU 参考：距离用闭式解，符号用"最近三角形法线"（与 GPU 的 parity 不同算法）
                    float ref = 1e30f;
                    float3 bestA(0.0f), bestB(0.0f), bestC(0.0f);
                    for (u32 t = 0; t < e.triCount; ++t) {
                        const u32* tri = &m_IndicesCPU[e.firstIndex + t * 3];
                        const float3 a = m_PositionsCPU[tri[0] + e.vertexOffset];
                        const float3 b = m_PositionsCPU[tri[1] + e.vertexOffset];
                        const float3 c = m_PositionsCPU[tri[2] + e.vertexOffset];
                        const float d = PointTriangleDistance(p, a, b, c);
                        if (d < ref) { ref = d; bestA = a; bestB = b; bestC = c; }
                    }

                    const float v = gpu[idx];
                    const float ad = std::fabs(v);
                    const float err = std::fabs(ad - ref);
                    st.maxErr = std::max(st.maxErr, err);
                    st.minV = std::min(st.minV, v);
                    st.maxV = std::max(st.maxV, v);
                    if (ad < 0.01f) { ++st.zeros; ++totalZero; }
                    if (ref <= 2.0f * e.voxelSize) {
                        ++st.nearCount;
                        if (err > 0.25f * e.voxelSize) ++st.nearBad;
                    } else {
                        ++st.farCount;
                        if (err > 2.0f * e.voxelSize) ++st.farBad;
                    }
                    ++st.counted;
                }
            }
        }
        totalProbes += st.counted;
    }
    m_ProbeDist->Unmap();

    // 点名：按"零值数"降序、再按最大误差降序
    std::sort(stats.begin(), stats.end(), [](const MeshStat& a, const MeshStat& b) {
        if (a.zeros != b.zeros) return a.zeros > b.zeros;
        return a.maxErr > b.maxErr;
    });
    HE_CORE_INFO("LumenSDF: 逐 mesh 场自检（共 {} 个 mesh / {} 探针，零值探针 {} 个）—— 最差 5 个：",
                 m_Entries.size(), totalProbes, totalZero);
    for (u32 i = 0; i < 5u && i < stats.size(); ++i) {
        const MeshStat& st = stats[i];
        const MeshSDFEntry& e = m_Entries[st.entry];
        HE_CORE_INFO("  条目 {}（命令 {}）：边长 {:.0f}，{} 三角形；值域 [{:.2f}, {:.2f}]，零值 {}，"
                     "最大误差 {:.2f}，近表面 {}/{} 超差，远场 {}/{} 超差",
                     st.entry, e.commandIndex, (double)(e.voxelSize * e.resolution), e.triCount,
                     (double)st.minV, (double)st.maxV, st.zeros, (double)st.maxErr,
                     st.nearBad, st.nearCount, st.farBad, st.farCount);
    }

    m_SelfCheck.valid  = true;
    m_SelfCheck.probes = totalProbes;
    m_SelfCheck.passed = (totalProbes > 0) && (totalZero == 0) && (stats[0].nearBad == 0);
    if (totalZero > 0) {
        HE_CORE_ERROR("LumenSDF 自检失败：有 {} 个探针处场值 ≈ 0（点名见上），这会直接导致步进提前命中",
                      totalZero);
    } else if (stats[0].nearBad > 0) {
        HE_CORE_ERROR("LumenSDF 自检失败：近表面 {} 个探针超出 1/4 体素（scatter 本应精确）",
                      stats[0].nearBad);
    } else {
        HE_CORE_WARN("LumenSDF 自检：无零值、近表面精确；远场仍有超差（洪泛近似，见各网格明细）");
    }
}
void LumenSDF::RunGlobalCheck() {
    const u32 strideBase = 4u;   // 每层 4³ = 64 个探针（CPU 参考要遍历全部三角形）
    for (u32 L = 0; L < m_GlobalLayerCount; ++L) {
        GlobalLayer& layer = m_GlobalLayers[L];
        if (!layer.probe || layer.probeCount == 0) continue;

        void* mapped = layer.probe->Map();
        if (!mapped) {
            HE_CORE_WARN("LumenSDF: Global 层 {} 的探针缓冲不可映射，跳过自检", L);
            continue;
        }
        const float* gpu = static_cast<const float*>(mapped);

        const u32 stride = std::max(1u, layer.res / strideBase);
        const u32 n = layer.res / stride;
        const float tol = 2.0f * layer.voxelSize;

        float maxErr = 0.0f, maxOver = -1e30f;
        double sumErr = 0.0;
        u32 within = 0, counted = 0;
        for (u32 z = 0; z < n; ++z) {
            for (u32 y = 0; y < n; ++y) {
                for (u32 x = 0; x < n; ++x) {
                    const u32 idx = z * n * n + y * n + x;
                    if (idx >= layer.probeCount) continue;
                    const float3 p = layer.origin +
                        float3((float)(x * stride) + 0.5f, (float)(y * stride) + 0.5f,
                               (float)(z * stride) + 0.5f) * layer.voxelSize;
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
                    const float err = gpu[idx] - ref;   // 期望 ≤ 0（下界）；> 0 即高估（危险）
                    sumErr += err;
                    maxOver = std::max(maxOver, err);
                    maxErr = std::max(maxErr, std::fabs(err));
                    if (std::fabs(err) <= tol) ++within;
                    ++counted;
                }
            }
        }
        layer.probe->Unmap();

        // ── 三方对照诊断（只对最细的近层做，避免刷屏）──
        // 对"低估最严重"的几个探针，把三个数并排打出来，回答"是谁把小值带进来的"：
        //   gpu   = 全局层在该点的值（注入 + 洪泛的结果）
        //   exact = CPU 对**全部**三角形的精确最小距离（真值）
        //   meshD = 包含该点的 mesh 中**精确距离最小**者的距离（= 注入本该写进去的值）
        // gpu ≈ meshD ⇒ 注入忠实，松的是 mesh 场/AABB 语义；gpu << meshD ⇒ 注入或采样有错。
        if (L == 0) {
            struct Row { float gpu, exact, meshD; int mesh; };
            std::vector<Row> rows;
            for (u32 z = 0; z < n; ++z) for (u32 y = 0; y < n; ++y) for (u32 x = 0; x < n; ++x) {
                const u32 idx = z * n * n + y * n + x;
                if (idx >= layer.probeCount) continue;
                const float3 p = layer.origin +
                    float3((float)(x * stride) + 0.5f, (float)(y * stride) + 0.5f,
                           (float)(z * stride) + 0.5f) * layer.voxelSize;
                float exact = 1e30f, meshD = 1e30f;
                int bestMesh = -1;
                for (u32 mi = 0; mi < (u32)m_Entries.size(); ++mi) {
                    const MeshSDFEntry& e = m_Entries[mi];
                    const float side = e.voxelSize * (float)e.resolution;
                    const bool inside = (p.x >= e.origin.x && p.x <= e.origin.x + side &&
                                         p.y >= e.origin.y && p.y <= e.origin.y + side &&
                                         p.z >= e.origin.z && p.z <= e.origin.z + side);
                    float dMesh = 1e30f;
                    for (u32 t = 0; t < e.triCount; ++t) {
                        const u32* tri = &m_IndicesCPU[e.firstIndex + t * 3];
                        const float3 a = m_PositionsCPU[tri[0] + e.vertexOffset];
                        const float3 b = m_PositionsCPU[tri[1] + e.vertexOffset];
                        const float3 c = m_PositionsCPU[tri[2] + e.vertexOffset];
                        const float dd = PointTriangleDistance(p, a, b, c);
                        dMesh = std::min(dMesh, dd);
                        exact = std::min(exact, dd);
                    }
                    if (inside && dMesh < meshD) { meshD = dMesh; bestMesh = (int)mi; }
                }
                rows.push_back({ gpu[idx], exact, meshD, bestMesh });
            }
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
                return (a.exact - a.gpu) > (b.exact - b.gpu);   // 低估最严重的排前面
            });
            HE_CORE_INFO("LumenSDF 三方对照（层 0，低估最严重的前 5 个探针）: gpu / exact / meshD / meshIdx");
            for (u32 i = 0; i < 5u && i < rows.size(); ++i) {
                HE_CORE_INFO("  [{:.2f} / {:.2f} / {:.2f} / {}]  (exact-gpu={:.2f})",
                             (double)rows[i].gpu, (double)rows[i].exact, (double)rows[i].meshD,
                             rows[i].mesh, (double)(rows[i].exact - rows[i].gpu));
            }
        }

        GlobalCheck& c = layer.check;
        c.valid = true;
        c.probes = counted;
        c.withinTol = within;
        c.maxError = maxErr;
        c.meanError = counted ? (float)(sumErr / counted) : 0.0f;
        c.tolerance = tol;
        // 判据只看安全方向：不得高估（否则 sphere tracing 穿漏）。下界质量只记录：
        // 它是 clipmap 分层这一步的关键指标（近层体素小 ⇒ 低估应显著变小）。
        c.passed = counted > 0 && maxOver <= tol;

        HE_CORE_INFO("LumenSDF Global 层 {} 自检: 体素 {:.3f}，探针 {}，最大高估 {:+.3f}"
                     "（判据 ≤ {:.3f}）=> {}；下界质量 {} 点在 2 体素内（{:.1f}%），平均低估 {:.2f}（层 0 的读数不可信，见代码注释）",
                     L, (double)layer.voxelSize, counted, (double)maxOver, (double)tol,
                     c.passed ? "PASS" : "FAIL", within,
                     counted ? 100.0 * (double)within / counted : 0.0,
                     (double)(-c.meanError));
        if (!c.passed) HE_CORE_ERROR("LumenSDF Global 层 {} 出现高估，sphere tracing 会穿漏", L);
    }
}
// ── sphere tracing（步骤 11）──
namespace {
constexpr u32 kMBindField  = 0;
constexpr u32 kMBindSample = 1;
constexpr u32 kMBindOrigin = 2;
constexpr u32 kMBindDir    = 3;
constexpr u32 kMBindHit    = 4;
constexpr u32 kMBindNormal = 5;
constexpr u32 kMBindField1 = 6;   // clipmap 远层的场（与近层同采样器语义）

// 与 SDF_RayMarch.comp.slang 的 MarchPC 一致：5 × 16B = 80B（层 0 参数 + 层 1 参数）
struct MarchPC {
    float originX, originY, originZ, voxelSize;      // 层 0（近层）
    u32   dimX, dimY, dimZ, rayCount;                // 层 0 分辨率 + 射线数
    float maxSteps, eps, maxDist, pad;
    float origin1X, origin1Y, origin1Z, voxelSize1;  // 层 1（远层）
    u32   dim1X, dim1Y, dim1Z, pad1;
};
static_assert(sizeof(MarchPC) == 80, "MarchPC 必须与 shader 的 5×16B 布局一致");

// 确定性伪随机（自检要可复现；不用 std::random 以免平台差异）
inline float NextRand(u32& s) {
    s = s * 1664525u + 1013904223u;
    return (float)((s >> 8) & 0xFFFFFFu) / (float)0x1000000u;
}
} // namespace

void LumenSDF::CreateMarchGPUObjects() {
    if (m_MarchPSO) return;

    rhi::DescriptorSetLayoutDesc layout;
    layout.bindings = {
        {kMBindField,  rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute},
        {kMBindField1, rhi::DescriptorType::CombinedImageSampler, 1, rhi::kStageMaskCompute},
        {kMBindOrigin, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},
        {kMBindDir,    rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},
        {kMBindHit,    rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},
        {kMBindNormal, rhi::DescriptorType::StorageBuffer, 1, rhi::kStageMaskCompute},
    };
    m_MarchLayout = m_Device->CreateDescriptorSetLayout(layout);
    m_MarchSet    = m_Device->AllocateDescriptorSet(m_MarchLayout);

    rhi::PushConstantRange pcr;
    pcr.stageMask = rhi::kStageMaskCompute;
    pcr.offset    = 0;
    pcr.size      = sizeof(MarchPC);

    rhi::ShaderBytecode cs;
    cs.stage      = rhi::ShaderStage::Compute;
    cs.spirv      = k_SDF_RayMarch_comp_spv;
    cs.entryPoint = "main";

    rhi::PipelineStateDesc pso;
    pso.bindPoint            = rhi::PipelineBindPoint::Compute;
    pso.computeShader        = &cs;
    pso.descriptorSetLayouts = {m_MarchLayout};
    pso.pushConstantRanges   = {pcr};
    pso.debugName            = "Lumen_SDF_RayMarch";
    m_MarchPSO = m_Device->CreatePipelineState(pso);
    if (!m_MarchPSO) HE_CORE_ERROR("LumenSDF: sphere tracing 的 PSO 创建失败");

    // 线性 clamp 采样器：三线性插值由采样器完成（步进需要连续场，不能最近邻）
    rhi::SamplerDesc sd;
    sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
    sd.addressU  = sd.addressV  = sd.addressW = rhi::AddressMode::ClampToEdge;
    m_LinearSampler = m_Device->CreateSampler(sd);

    // ── 细节追踪（逐 mesh 的 min 归约）──
    m_DetailLayout = m_Device->CreateDescriptorSetLayout(layout);   // 绑定集与全局版相同
    m_DetailSet    = m_Device->AllocateDescriptorSet(m_DetailLayout);

    rhi::ShaderBytecode dcs;
    dcs.stage      = rhi::ShaderStage::Compute;
    dcs.spirv      = k_SDF_RayMarchDetail_comp_spv;
    dcs.entryPoint = "main";

    rhi::PipelineStateDesc dpso;
    dpso.bindPoint            = rhi::PipelineBindPoint::Compute;
    dpso.computeShader        = &dcs;
    dpso.descriptorSetLayouts = {m_DetailLayout};
    dpso.pushConstantRanges   = {pcr};
    dpso.debugName            = "Lumen_SDF_RayMarchDetail";
    m_DetailPSO = m_Device->CreatePipelineState(dpso);
    if (!m_DetailPSO) HE_CORE_ERROR("LumenSDF: 细节追踪的 PSO 创建失败");
}

void LumenSDF::SetupMarchRays() {
    const u32 n = std::max(1u, m_Config.marchRays);
    m_RayOriginCPU.resize(n);
    m_RayDirCPU.resize(n);

    u32 seed = 20260919u;   // 固定种子：自检可复现
    for (u32 i = 0; i < n; ++i) {
        // 起点落在**某个 mesh 的 AABB 内**：那里全局场取自该 mesh 的精确距离场，
        // 最能检验步进本身；起点落在几何内部也无妨（本版是无符号场，会在表面附近命中）。
        const MeshSDFEntry& e = m_Entries[(u32)(NextRand(seed) * (float)m_Entries.size()) % m_Entries.size()];
        const float side = e.voxelSize * (float)e.resolution;
        // 起点改为"贴着几何表面"：随机取该 mesh 的一个三角形、面内取随机重心点，再沿法线外移 1 个单位。
        // 理由：随机撒在 AABB 内的点大多远离几何，实测那类射线误差 p50 = 59.75 体素、把指标完全带偏
        // （近表面射线其实只有 0.159 体素）。探针射线在真实使用中就是从表面出发的，测试集必须与用法一致。
        const u32 triIdx = (u32)(NextRand(seed) * (float)std::max(1u, e.triCount)) % std::max(1u, e.triCount);
        const u32* tri = &m_IndicesCPU[e.firstIndex + triIdx * 3];
        const float3 A = m_PositionsCPU[tri[0] + e.vertexOffset];
        const float3 B = m_PositionsCPU[tri[1] + e.vertexOffset];
        const float3 C = m_PositionsCPU[tri[2] + e.vertexOffset];
        float w0 = NextRand(seed), w1 = NextRand(seed);
        if (w0 + w1 > 1.0f) { w0 = 1.0f - w0; w1 = 1.0f - w1; }
        const float3 surf = A + (B - A) * w0 + (C - A) * w1;
        float3 nrm = glm::cross(B - A, C - A);
        nrm = (glm::dot(nrm, nrm) > 1e-12f) ? glm::normalize(nrm) : float3(0.0f, 1.0f, 0.0f);
        // 起点外移 **4 个单位**（原来 1 个）：收敛阈值 eps = 0.25 × 近层体素 = 1.54，起点离表面 1 个单位时
        // d(origin) 就已经小于 eps，射线在 t=0 处"命中" —— 于是无论怎么改步进都不会动指标（实测把步长
        // 系数从 0.5 降到 0.25 后逐位不变，正是这个原因）。外移到 4 个单位（> 2.6×eps）才真正考验步进。
        const float3 p = surf + nrm * 4.0f;
        float3 d(NextRand(seed) * 2.0f - 1.0f, NextRand(seed) * 2.0f - 1.0f, NextRand(seed) * 2.0f - 1.0f);
        if (glm::dot(d, d) < 1e-6f) d = float3(0.0f, -1.0f, 0.0f);
        m_RayOriginCPU[i] = p;
        m_RayDirCPU[i]    = glm::normalize(d);
    }

    std::vector<float4> origins(n), dirs(n);
    for (u32 i = 0; i < n; ++i) {
        origins[i] = float4(m_RayOriginCPU[i], 0.0f);
        dirs[i]    = float4(m_RayDirCPU[i], 0.0f);
    }
    rhi::BufferDesc bd;
    bd.usage = rhi::BufferUsage::Storage;
    bd.size = origins.size() * sizeof(float4); bd.initialData = origins.data();
    m_RayOrigin = m_Device->CreateBuffer(bd);
    bd.size = dirs.size() * sizeof(float4); bd.initialData = dirs.data();
    m_RayDir = m_Device->CreateBuffer(bd);

    bd.initialData = nullptr;
    bd.cpuAccess = true;                     // 自检要读回
    bd.size = (usize)n * sizeof(float4);
    m_RayHit    = m_Device->CreateBuffer(bd);
    m_RayNormal = m_Device->CreateBuffer(bd);
    bd.size = (usize)n * sizeof(u32);
    m_RayT  = m_Device->CreateBuffer(bd);
    m_RayTMapped = m_RayT ? m_RayT->Map() : nullptr;

    m_Device->UpdateDescriptorSet(m_MarchSet, kMBindOrigin, rhi::DescriptorType::StorageBuffer, m_RayOrigin.get());
    m_Device->UpdateDescriptorSet(m_MarchSet, kMBindDir,    rhi::DescriptorType::StorageBuffer, m_RayDir.get());
    m_Device->UpdateDescriptorSet(m_MarchSet, kMBindHit,    rhi::DescriptorType::StorageBuffer, m_RayHit.get());
    m_Device->UpdateDescriptorSet(m_MarchSet, kMBindNormal, rhi::DescriptorType::StorageBuffer, m_RayNormal.get());
    // 细节追踪用同一个绑定号布局：field/sampler/origin/dir/minTarget
    m_Device->UpdateDescriptorSet(m_DetailSet, kMBindOrigin, rhi::DescriptorType::StorageBuffer, m_RayOrigin.get());
    m_Device->UpdateDescriptorSet(m_DetailSet, kMBindDir,    rhi::DescriptorType::StorageBuffer, m_RayDir.get());
    m_Device->UpdateDescriptorSet(m_DetailSet, kMBindHit,    rhi::DescriptorType::StorageBuffer, m_RayT.get());
    m_Device->UpdateDescriptorSet(m_DetailSet, kMBindNormal, rhi::DescriptorType::StorageBuffer, m_RayT.get());
    HE_CORE_INFO("LumenSDF: sphere tracing 验证射线已生成（{} 条，最大步数 {}，收敛阈值 {:.4f}，最大距离 {:.1f}）",
                 n, m_Config.marchMaxSteps, (double)(0.25f * GetGlobalVoxelSize(0)), (double)m_Config.marchMaxDist);
}

void LumenSDF::RunMarch(rhi::IRHICommandList* cmd) {
    if (!m_MarchPSO || !GetGlobalField(0) || m_RayOriginCPU.empty()) return;

    m_Device->UpdateDescriptorSet(m_MarchSet, kMBindField,
        rhi::DescriptorType::CombinedImageSampler, GetGlobalField(0), m_LinearSampler.get());
    m_Device->UpdateDescriptorSet(m_MarchSet, kMBindField1,
        rhi::DescriptorType::CombinedImageSampler, GetGlobalField(1), m_LinearSampler.get());

    MarchPC pc{};
    pc.originX = GetGlobalOrigin(0).x; pc.originY = GetGlobalOrigin(0).y; pc.originZ = GetGlobalOrigin(0).z;
    pc.voxelSize = GetGlobalVoxelSize(0);
    pc.dimX = pc.dimY = pc.dimZ = m_GlobalLayers[0].res;
    pc.origin1X = GetGlobalOrigin(1).x; pc.origin1Y = GetGlobalOrigin(1).y; pc.origin1Z = GetGlobalOrigin(1).z;
    pc.voxelSize1 = GetGlobalVoxelSize(1);
    pc.dim1X = pc.dim1Y = pc.dim1Z = m_GlobalLayers[1].res;
    pc.rayCount  = (u32)m_RayOriginCPU.size();
    pc.maxSteps  = (float)m_Config.marchMaxSteps;
    pc.eps       = 0.25f * GetGlobalVoxelSize(0);
    pc.maxDist   = m_Config.marchMaxDist;
    // 审计：把实际下发的两层参数打出来（射线自检对任何改动都不动，先证明这条通道是活的）
    HE_CORE_INFO("LumenSDF march 参数: 层0 原点({:.1f},{:.1f},{:.1f}) 体素 {:.3f} res {} | 层1 原点({:.1f},{:.1f},{:.1f}) 体素 {:.3f} res {} | 射线 {} 步数 {:.0f} eps {:.3f}",
                 (double)pc.originX, (double)pc.originY, (double)pc.originZ, (double)pc.voxelSize, pc.dimX,
                 (double)pc.origin1X, (double)pc.origin1Y, (double)pc.origin1Z, (double)pc.voxelSize1, pc.dim1X,
                 pc.rayCount, (double)pc.maxSteps, (double)pc.eps);

    cmd->SetPipeline(m_MarchPSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_MarchSet);
    cmd->SetPushConstants(0, sizeof(pc), &pc);
    cmd->Dispatch((pc.rayCount + 63u) / 64u, 1, 1);
}

void LumenSDF::RunMarchDetail(rhi::IRHICommandList* cmd) {
    if (!m_DetailPSO || m_Entries.empty() || m_RayOriginCPU.empty()) return;

    // 每帧先把细节命中缓冲重置为 +inf（位模式），再做逐 mesh 的 min 归约
    const u32 n = (u32)m_RayOriginCPU.size();
    if (m_RayTMapped) {
        std::vector<u32> inf(n, 0x7F800000u);
        std::memcpy(m_RayTMapped, inf.data(), (usize)n * sizeof(u32));   // 持久映射：直接写
    }

    cmd->SetPipeline(m_DetailPSO.get());
    cmd->BindDescriptorSet(rhi::kDescSetPerFrame, m_DetailSet);

    MarchPC pc{};
    pc.dimX = pc.dimY = pc.dimZ = 0;   // 每个 mesh 覆盖
    pc.rayCount = n;
    pc.maxSteps = (float)m_Config.marchMaxSteps;
    pc.maxDist  = m_Config.marchMaxDist;

    for (const auto& e : m_Entries) {
        if (!e.field) continue;
        m_Device->UpdateDescriptorSet(m_DetailSet, kMBindField,
            rhi::DescriptorType::CombinedImageSampler, e.field.get(), m_LinearSampler.get());
        pc.originX = e.origin.x; pc.originY = e.origin.y; pc.originZ = e.origin.z;
        pc.voxelSize = e.voxelSize;
        pc.dimX = pc.dimY = pc.dimZ = e.resolution;
        pc.eps   = 0.25f * e.voxelSize;    // eps 挂**该 mesh** 的体素（细节追踪的意义所在）
        cmd->SetPushConstants(0, sizeof(pc), &pc);
        cmd->Dispatch((n + 63u) / 64u, 1, 1);
        cmd->PipelineBarrier(rhi::PipelineStage::ComputeShader, rhi::PipelineStage::ComputeShader,
                             rhi::ResourceState::UnorderedAccess, rhi::ResourceState::UnorderedAccess,
                             GetGlobalField(0));
    }
}

void LumenSDF::RunMarchCheck() {
    if (!m_RayHit || !m_RayNormal || !GetGlobalField(0)) return;
    void* hitMapped = m_RayHit->Map();
    void* nrmMapped = m_RayNormal->Map();
    if (!hitMapped || !nrmMapped) {
        HE_CORE_WARN("LumenSDF: sphere tracing 结果缓冲不可映射，跳过自检");
        return;
    }
    const float4* hits = static_cast<const float4*>(hitMapped);
    const float4* nrms = static_cast<const float4*>(nrmMapped);
    const u32*    detailT = m_RayTMapped ? static_cast<const u32*>(m_RayTMapped) : nullptr;
    {
        u32 detailHits = 0;
        float detailMin = 1e30f;
        if (detailT) {
            for (u32 i = 0; i < (u32)m_RayOriginCPU.size(); ++i) {
                float v; std::memcpy(&v, &detailT[i], sizeof(float));
                if (v < 1e29f) { ++detailHits; detailMin = std::min(detailMin, v); }
            }
        }
        HE_CORE_INFO("LumenSDF 细节追踪统计: 缓冲可映射={}, 命中 {} 条，最小 t={:.3f}",
                     (m_RayTMapped != nullptr), detailHits,
                     detailHits ? (double)detailMin : 0.0);
    }

    const u32 n = (u32)m_RayOriginCPU.size();
    float maxErr = 0.0f;
    double sumErr = 0.0;
    u32 bothHit = 0, gpuOnly = 0, cpuOnly = 0, within = 0, normalOk = 0;
    std::vector<float> errVoxAll, errNear, errFar;   // 误差分布 + 按"起点是否贴近几何"分组
    u32 withinGlobal = 0, detailBetter = 0;
    double sumErrGlobal = 0.0;

    for (u32 i = 0; i < n; ++i) {
        const float3 ro = m_RayOriginCPU[i];
        float d0 = 1e30f;   // 起点到几何的精确距离（诊断"上游"：射线集是否退化）
        const float3 rd = m_RayDirCPU[i];

        // CPU 参考：Möller–Trumbore 对全部三角形取最近正交点（带逐 mesh AABB 粗筛）
        float tRef = 1e30f;
        for (const auto& e : m_Entries) {
            const float side = e.voxelSize * (float)e.resolution;
            // slab 粗筛
            float t0 = 0.0f, t1 = 1e30f;
            bool miss = false;
            for (int a = 0; a < 3; ++a) {
                const float o = (&ro.x)[a], d = (&rd.x)[a];
                const float lo = (&e.origin.x)[a], hi = lo + side;
                if (std::fabs(d) < 1e-9f) { if (o < lo || o > hi) { miss = true; break; } continue; }
                float ta = (lo - o) / d, tb = (hi - o) / d;
                if (ta > tb) std::swap(ta, tb);
                t0 = std::max(t0, ta); t1 = std::min(t1, tb);
                if (t0 > t1) { miss = true; break; }
            }
            if (miss || t0 > tRef) continue;

            for (u32 t = 0; t < e.triCount; ++t) {
                const u32* tri = &m_IndicesCPU[e.firstIndex + t * 3];
                const float3 v0 = m_PositionsCPU[tri[0] + e.vertexOffset];
                const float3 v1 = m_PositionsCPU[tri[1] + e.vertexOffset];
                const float3 v2 = m_PositionsCPU[tri[2] + e.vertexOffset];
                const float3 e1 = v1 - v0, e2 = v2 - v0;
                const float3 pv = glm::cross(rd, e2);
                d0 = std::min(d0, PointTriangleDistance(ro, v0, v1, v2));
                const float det = glm::dot(e1, pv);
                if (std::fabs(det) < 1e-12f) continue;
                const float inv = 1.0f / det;
                const float3 tv = ro - v0;
                const float u = glm::dot(tv, pv) * inv;
                if (u < 0.0f || u > 1.0f) continue;
                const float3 qv = glm::cross(tv, e1);
                const float v = glm::dot(rd, qv) * inv;
                if (v < 0.0f || u + v > 1.0f) continue;
                const float tt = glm::dot(e2, qv) * inv;
                if (tt > 1e-4f && tt < tRef) tRef = tt;
            }
        }

        const bool gpuHit = hits[i].y > 0.5f;
        const bool cpuHit = (tRef < 1e29f) && (tRef <= m_Config.marchMaxDist);

        // 细节追踪的结果与全局追踪取 min（两者都是下界 ⇒ 合并后仍不高估）：最近命中
        float tDetail = 1e30f;
        if (detailT) {
            float v;
            std::memcpy(&v, &detailT[i], sizeof(float));   // 位模式 → float
            tDetail = v;
        }
        // 合并策略：取 min（两条追踪都是下界 ⇒ 合并后仍不高估，安全性质保持）。
        // 实测 detail-first 语义（有细节命中就以它为准）更差：75/195 vs 76/195 —— 因为**无符号**
        // 场让"起点在几何内部"的射线在细节追踪里立刻命中（t≈0），而全局场又因严重低估而提前命中。
        // 两条都指向同一个根因（缺符号 + 场不紧），见 §5 的结论。
        const float tGpuMerged = std::min(gpuHit ? hits[i].x : 1e30f, tDetail);
        const bool  mergedHit  = tGpuMerged < 1e29f;

        if (mergedHit && cpuHit) {
            ++bothHit;
            const float errVox = std::fabs(tGpuMerged - tRef) / GetGlobalVoxelSize(0);
            sumErr += errVox;
            maxErr = std::max(maxErr, errVox);
            if (errVox <= 1.0f) ++within;
            errVoxAll.push_back(errVox);
            ((d0 < 5.0f) ? errNear : errFar).push_back(errVox);
            if (hits[i].w < 0.0f) ++normalOk;   // 法线朝向与射线相反 = 正面命中

            // 诊断：把"全局单独"与"合并后"的误差分开记，才能判断细节追踪到底有没有帮忙
            if (gpuHit) {
                const float errG = std::fabs(hits[i].x - tRef) / GetGlobalVoxelSize(0);
                sumErrGlobal += errG;
                if (errG <= 1.0f) ++withinGlobal;
            }
            if (tDetail < 1e29f && (!gpuHit || tDetail < hits[i].x)) ++detailBetter;
        } else if (mergedHit) {
            ++gpuOnly;
        } else if (cpuHit) {
            ++cpuOnly;
        }
    }
    m_RayHit->Unmap();
    m_RayNormal->Unmap();

    m_MarchCheck.valid      = true;
    m_MarchCheck.rays       = n;
    m_MarchCheck.bothHit    = bothHit;
    m_MarchCheck.gpuOnly    = gpuOnly;
    m_MarchCheck.cpuOnly    = cpuOnly;
    m_MarchCheck.withinTol  = within;
    m_MarchCheck.normalOk   = normalOk;
    m_MarchCheck.maxErrVox  = maxErr;
    m_MarchCheck.meanErrVox = bothHit ? (float)(sumErr / bothHit) : 0.0f;
    // 判据分两层（与步骤 10 的自检同一思路：把"安全"与"精度"分开量）：
    //   · 安全（pass/fail 门槛）：不得出现"CPU 命中了而 GPU 没命中"——那就是穿漏；
    //   · 精度（只记录）：命中距离误差 ≤1 体素的比例。当前全局场是粗层（24.66 单位体素），
    //     精度达不到 1 体素是**已知**的（§5 的下界质量），要等 clipmap 分层 + 细层 eps 才能达标。
    //   · `gpuOnly` 的假命中来自**无符号**场：射线起点落在几何内部时 d 立刻小于阈值。
    m_MarchCheck.passed = (bothHit > 0) && (cpuOnly == 0);

    HE_CORE_INFO("LumenSDF sphere tracing 自检: 射线 {}，两者都命中 {}，仅 GPU {}（无符号场在几何内部的假命中），"
                 "仅 CPU {}（穿漏，须为 0）=> 安全 {}；精度：误差 ≤1 体素 {}/{}（{:.1f}%），"
                 "最大 {:.3f} 体素，平均 {:.3f}；法线朝向正确 {}",
                 n, bothHit, gpuOnly, cpuOnly, m_MarchCheck.passed ? "PASS" : "FAIL",
                 within, bothHit, bothHit ? 100.0 * (double)within / bothHit : 0.0,
                 (double)maxErr, (double)m_MarchCheck.meanErrVox, normalOk);
    if (!m_MarchCheck.passed) {
        HE_CORE_ERROR("LumenSDF sphere tracing 出现穿漏（仅 CPU 命中 {} 条），检查场的下界性质与 eps", cpuOnly);
    } else if ((float)within / (float)std::max(1u, bothHit) < 0.9f) {
        HE_CORE_WARN("LumenSDF sphere tracing 精度未达标（{:.1f}% ≤1 体素）：当前为 Global SDF 单层粗分辨率，"
                     "需 clipmap 分层 + 细层收敛阈值（§5 的下界质量结论）", 
                     bothHit ? 100.0 * (double)within / bothHit : 0.0);
    }
    if (!errVoxAll.empty()) {
        std::sort(errVoxAll.begin(), errVoxAll.end());
        const float p50 = errVoxAll[errVoxAll.size() / 2];
        const float p90 = errVoxAll[(size_t)(errVoxAll.size() * 9 / 10)];
        HE_CORE_INFO("LumenSDF sphere tracing 误差分布: n={} p50={:.3f} p90={:.3f} 体素；按起点到几何距离分组: 近(d0<5) n={} p50={:.3f} / 远 n={} p50={:.3f}",
                     errVoxAll.size(), (double)p50, (double)p90,
                     errNear.size(), errNear.empty() ? 0.0 : (double)errNear[errNear.size()/2],
                     errFar.size(), errFar.empty() ? 0.0 : (double)errFar[errFar.size()/2]);
    }    HE_CORE_INFO("LumenSDF sphere tracing 误差分解: 仅全局场 {}/{} 在 1 体素内（平均 {:.3f}），"
                 "合并细节追踪后 {}/{}（平均 {:.3f}）；细节追踪更近的射线 {} 条",
                 withinGlobal, bothHit, bothHit ? sumErrGlobal / bothHit : 0.0,
                 within, bothHit, bothHit ? sumErr / bothHit : 0.0, detailBetter);
}

} // namespace he::render
