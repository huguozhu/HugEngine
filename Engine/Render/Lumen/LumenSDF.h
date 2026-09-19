#pragma once

// ============================================================
// Lumen/LumenSDF.h — 逐 mesh 距离场构建（步骤 8）
//
// 【它在 Lumen 里的位置】§5 的 SDF 体系第一层：Mesh SDF →（步骤 10）注入 Global SDF →
// （步骤 11）sphere tracing 求交。本类只负责"每个 mesh 一张距离场"这一层。
//
// 【数据来源与坐标空间】几何取自 `MeshBatcher::GetMergedVertices/GetMergedIndices`
// （未施加变换的合并几何），配合 `GetDrawCommands()` 的 (firstIndex, vertexOffset) 切出
// 每个 mesh 的三角形区间；网格是**该 mesh 局部空间**的 AABB，与世界变换解耦（变换在 traced
// 阶段由实例数据提供，步骤 11）。
//
// 【首版的两处限制，都是刻意的】
//   1. 分辨率默认 32³（设计里的 128³ 需要 scatter + InterlockedMin 版本，见 shader 头注释）；
//   2. 每个 mesh 的三角形数有上限（gather 是 O(体素 × 三角形)），超过就不建这张场，
//      该网格回落到步骤 10 的 Global SDF 覆盖。
// 两条都记在 §12 的"实现前置"里，不是遗漏。
//
// 【自检（步骤 9 的一部分）】构建完第一个 mesh 后，把 shader 写进探针缓冲的距离读回，
// 与 CPU 侧用同一公式的独立实现逐点比对，误差超阈值就记 ERROR。它验证的是数据链路
// （缓冲布局 / 网格映射 / 描述符 / push constant）——这类错误的表现是静默的 0 或错位。
// ============================================================

#include "RHI/RHI.h"
#include "Pipeline/MeshBatcher.h"

#include <memory>
#include <vector>

namespace he::render {

/// 构建参数（默认值 = 首版默认项；每一步都可配，便于按机器调）
struct LumenSDFConfig {
    u32 resolution     = 32;     // 每 mesh 立方体素边长（32³ = 32768 体素）
    u32 maxMeshes      = 64;     // 显存上限（R32F：64 × 32³ × 4B = 8 MB；128³ 时是 537 MB）
    u32 maxTrisPerMesh = 4096;   // 超过则不建（gather 的代价随三角形数线性放大）
    u32 meshesPerFrame = 4;      // 每帧构建预算（避免一次卡顿）
    u32 probeStride    = 4;      // 自检采样步长（体素）
};

/// 一个 mesh 的距离场条目
struct MeshSDFEntry {
    u32   commandIndex = 0;      // MeshBatcher 命令下标（= GPUScene objectIndex）
    u32   firstIndex   = 0;      // 索引起始（命令里的 firstIndex）
    u32   indexCount   = 0;
    u32   vertexOffset = 0;
    u32   triCount     = 0;
    float3 origin      = float3(0.0f);   // 局部空间 AABB 最小角
    float voxelSize    = 0.0f;
    u32   resolution   = 0;
    u32   probeCount   = 0;
    std::unique_ptr<rhi::IRHITexture> field;   // R32F 3D 距离场
};

class LumenSDF {
public:
    bool Initialize(rhi::IRHIDevice* device, const LumenSDFConfig& config = {});
    void Shutdown();

    /// 每帧推进一次（由 LumenProvider::Render 调用）：建档 → 逐帧构建 → 读回自检。
    /// @param cmd    当前帧的命令列表（Lumen 的主 pass 里）
    /// @param batcher 本帧的合并几何（提供 CPU 侧顶点/索引与每 mesh 区间）
    void Step(rhi::IRHICommandList* cmd, const MeshBatcher& batcher);

    [[nodiscard]] bool IsReady()  const { return m_Device != nullptr; }
    [[nodiscard]] bool IsComplete() const { return m_Done; }
    [[nodiscard]] const LumenSDFConfig& GetConfig() const { return m_Config; }
    [[nodiscard]] const std::vector<MeshSDFEntry>& GetEntries() const { return m_Entries; }
    /// 已构建的场占用的显存（字节，按 voxel 数 × 4B 计）
    [[nodiscard]] u64 GetMemoryBytes() const;
    /// 自检结论（未跑完时 valid=false）
    struct SelfCheck {
        bool  valid    = false;
        bool  passed   = false;
        u32   probes   = 0;
        float maxError = 0.0f;   // 世界单位
        float tolerance = 0.0f;  // 阈值（= 0.25 × 体素边长）
    };
    [[nodiscard]] const SelfCheck& GetSelfCheck() const { return m_SelfCheck; }

private:
    void BuildQueue(const MeshBatcher& batcher);
    void UploadGeometry(const MeshBatcher& batcher);
    void CreateGPUObjects();
    void BakeOne(rhi::IRHICommandList* cmd, u32 entryIndex);
    void RunSelfCheck();
    static float PointTriangleDistance(const float3& p, const float3& a,
                                       const float3& b, const float3& c);

    rhi::IRHIDevice* m_Device = nullptr;
    LumenSDFConfig   m_Config;

    // GPU 对象
    std::unique_ptr<rhi::IRHIPipelineState>   m_PSO;
    rhi::DescriptorSetLayoutHandle            m_Layout;
    rhi::DescriptorSetHandle                  m_Set;
    std::unique_ptr<rhi::IRHIBuffer>          m_Positions;   // float4（w 未用）
    std::unique_ptr<rhi::IRHIBuffer>          m_Indices;
    std::unique_ptr<rhi::IRHIBuffer>          m_ProbeDist;   // CPU 可读（自检）
    u32 m_ProbeCount = 0;

    // 状态机
    enum class Phase { Idle, Baking, WaitSelfCheck, Done };
    Phase m_Phase = Phase::Idle;
    u32   m_Frame = 0;        // Step 调用计数（自增，与飞行帧槽位无关）
    u32   m_NextEntry = 0;    // 下一个待构建的条目
    u32   m_WaitFrames = 0;   // 自检前的等待帧数（等 GPU 写完探针缓冲）
    bool  m_GeometryLogged = false;
    bool  m_Done = false;
    bool  m_GeometryUploaded = false;

    std::vector<MeshSDFEntry> m_Entries;
    SelfCheck m_SelfCheck;

    // CPU 侧几何副本（自检用：与 GPU 走完全不同的数据路径，才能验证映射/偏移正确）
    std::vector<float3> m_PositionsCPU;
    std::vector<u32>    m_IndicesCPU;
};

} // namespace he::render
