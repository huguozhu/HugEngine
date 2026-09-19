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
    u32 resolution     = 128;    // 每 mesh 立方体素边长（scatter 版：代价 O(表面 × 分辨率²)）。
                                 // 实测 160³ 相对 128³：远场误差与 sphere tracing 精度都不变、显存 128→250 MB、
                                 // 烘焙 6→20 s ⇒ 已回退；瓶颈不在 mesh 层分辨率。
    u32 maxMeshes      = 16;     // 显存上限（R32F：16 × 128³ × 4B = 134 MB；32³ 时同样 16 个只要 2 MB）
    u32 maxTrisPerMesh = 20000;  // scatter 与三角形数线性，上限可远高于 gather 版
    u32 meshesPerFrame = 2;      // 每帧构建预算（128³ 的 scatter + convert 更重）
    u32 probeStride    = 0;      // 自检采样步长（0 = 自动取 resolution/4）
    u32 globalResolution = 128;  // Global SDF 单层分辨率（clipmap 分层留待后续步骤）
    u32 globalLayers     = 2;    // clipmap 层数（1 = 旧行为：单层覆盖全场；2 = 近层 + 远层）
    float nearFraction   = 0.50f;// 近层边长 = 场景最长轴 × 该比例（近层体素 ≈ 远层 / 比例）
    // ── sphere tracing 验证（步骤 11）──
    u32   marchRays     = 256;   // 验证用射线数
    u32   marchMaxSteps = 384;   // 最大步数（设计写 64；下界质量不足时步数会更费，见 §5）
    float marchMaxDist  = 400.0f;// 最大追踪距离（世界单位）
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

    // ── Global SDF（步骤 10 / clipmap 分层）──
    /// 指定层的可采样全局场（0 = 最细的近层，kGlobalLayerCount-1 = 覆盖全场的远层）
    [[nodiscard]] rhi::IRHITexture* GetGlobalField(u32 layer = kMaxGlobalLayers - 1u) const {
        return (layer < m_GlobalLayerCount) ? m_GlobalLayers[layer].field.get() : nullptr;
    }
    [[nodiscard]] u32    GetGlobalLayerCount() const { return m_GlobalLayerCount; }
    [[nodiscard]] float  GetGlobalVoxelSize(u32 layer = kMaxGlobalLayers - 1u) const {
        return (layer < m_GlobalLayerCount) ? m_GlobalLayers[layer].voxelSize : 0.0f;
    }
    [[nodiscard]] float3 GetGlobalOrigin(u32 layer = kMaxGlobalLayers - 1u) const {
        return (layer < m_GlobalLayerCount) ? m_GlobalLayers[layer].origin : float3(0.0f);
    }
    /// Global SDF 的自检结论（未跑完时 valid=false）
    struct GlobalCheck {
        bool  valid     = false;
        bool  passed    = false;
        u32   probes    = 0;
        u32   withinTol = 0;      // 落在容差内的探针数
        float maxError  = 0.0f;   // max(GPU - CPU)（本版近似只可能偏大）
        float meanError = 0.0f;
        float tolerance = 0.0f;   // = 2 × 全局体素边长
    };
    /// 远层（覆盖全场那层）的自检结论（逐层结果见日志）
    [[nodiscard]] const GlobalCheck& GetGlobalCheck() const {
        return m_GlobalLayers[kMaxGlobalLayers - 1u].check;
    }

    // ── sphere tracing（步骤 11）──
    /// sphere tracing 的自检结论：GPU 命中距离 vs CPU 精确射线-三角形求交
    struct MarchCheck {
        bool  valid      = false;
        bool  passed     = false;
        u32   rays       = 0;
        u32   bothHit    = 0;
        u32   gpuOnly    = 0;      // GPU 命中而 CPU 未命中（不该发生）
        u32   cpuOnly    = 0;      // CPU 命中而 GPU 未命中（步数/下界质量不足）
        u32   withinTol  = 0;      // 两者都命中且 |Δt| ≤ 1 体素
        u32   normalOk   = 0;      // 法线朝向与射线方向相反（dot < 0）
        float maxErrVox  = 0.0f;   // 最大误差（体素）
        float meanErrVox = 0.0f;
    };
    [[nodiscard]] const MarchCheck& GetMarchCheck() const { return m_MarchCheck; }

    // ── SDF 追踪可视化（步骤 12，L1 退出判据）──
    /// 相机位置：clipmap 的**近层跟随相机**（UE 的做法）。必须在第一次 Step 之前设置，
    /// 否则近层会退化成"以场景中心为心的盒子"——实测那块盒子里几乎没有几何（§附六）。
    void SetCameraPos(const float3& p) { m_CameraPos = p; m_CameraPosSet = true; }
    /// 视口尺寸：调试视图按屏幕分辨率逐像素发射主射线（由 LumenScene 在 Initialize/OnResize 时同步）
    void SetViewport(u32 width, u32 height) {
        if (width == m_ViewportW && height == m_ViewportH) return;
        m_ViewportW = width;
        m_ViewportH = height;
        m_DebugTex.reset();   // 尺寸变了：输出纹理必须重建（PSO / 布局与尺寸无关，不重建）
    }
    /// 每帧执行一次逐像素 sphere tracing，把结果写进调试纹理（由帧图的 SDF compute pass 调用）。
    /// 相机参数逐帧变化，故不缓存；矩阵类转置风险用基向量绕开（见 shader 注释）。
    void RunDebugView(rhi::IRHICommandList* cmd,
                      const float3& camPos, const float3& forward, const float3& right, const float3& up,
                      float tanHalfFov, float aspect);
    /// 调试视图输出（RGBA16F，屏幕尺寸）：R=命中层, G=t/最大距离, B=步数比, A=是否命中
    [[nodiscard]] rhi::IRHITexture* GetDebugTexture() const { return m_DebugTex.get(); }

private:
    void BuildQueue(const MeshBatcher& batcher);
    void UploadGeometry(const MeshBatcher& batcher);
    void CreateGPUObjects();
    void BakeOne(rhi::IRHICommandList* cmd, u32 entryIndex);
    void RunSelfCheck();
    // ── Global SDF（步骤 10）──
    void CreateGlobalGPUObjects();
    void SetupGlobalGrid();
    void BuildGlobalField(rhi::IRHICommandList* cmd);
    void RunGlobalCheck();
    // ── sphere tracing（步骤 11）──
    void CreateMarchGPUObjects();
    void SetupMarchRays();
    void RunMarch(rhi::IRHICommandList* cmd);
    void RunMarchDetail(rhi::IRHICommandList* cmd);   // 逐 mesh 细节追踪（min 归约到 u_RayT）
    void RunMarchCheck();
    // ── SDF 追踪可视化（步骤 12）──
    void CreateDebugGPUObjects();
    void LogDebugStats();
    /// 逐帧探测单个 mesh 场在相机处的值（定位"哪个 mesh 场在空旷处为 0"）
    void RunMeshFieldProbe(rhi::IRHICommandList* cmd, const float3& camPos, const float3& fwd);
    static float PointTriangleDistance(const float3& p, const float3& a,
                                       const float3& b, const float3& c);
    /// 点 p 到全部几何的精确距离（逐 mesh AABB 粗筛）：自检与调试视图共用的"真值"查询
    [[nodiscard]] float MinDistToGeometry(const float3& p) const;
    static float3 ClosestPointOnTriangle(const float3& p, const float3& a,
                                         const float3& b, const float3& c);

    rhi::IRHIDevice* m_Device = nullptr;
    LumenSDFConfig   m_Config;

    // GPU 对象
    std::unique_ptr<rhi::IRHIPipelineState>   m_PSO;          // scatter 版：清空/scatter
    rhi::DescriptorSetLayoutHandle            m_Layout;
    rhi::DescriptorSetHandle                  m_Set;
    std::unique_ptr<rhi::IRHIPipelineState>   m_ConvertPSO;   // u32 → R32F + 探针
    std::unique_ptr<rhi::IRHIPipelineState>   m_FloodPSO;     // 跳步洪泛（补全 scatter 的空洞）
    rhi::DescriptorSetLayoutHandle            m_ConvertLayout;
    rhi::DescriptorSetHandle                  m_ConvertSet;
    // 同上：转换 pass 的输出是**每 mesh 一张**纹理，共用一套描述符集会让写入落到同一张上
    std::vector<rhi::DescriptorSetHandle>     m_ConvertSets;
    std::unique_ptr<rhi::IRHITexture>         m_MeshScratch;  // 共享的 u32 距离场（原子最小目标）
    // 带种子坐标的 JFA（真欧氏距离，见 SDF_MeshFloodSeeds.comp.slang）：种子坐标共享一张纹理，
    // 逐 mesh 串行复用（+8 MB），把"26 连通图最短路径"的 ~8% 高估换成 ~1 体素的精确欧氏距离。
    std::unique_ptr<rhi::IRHITexture>         m_MeshSeed;
    rhi::DescriptorSetLayoutHandle            m_SeedFloodLayout;
    rhi::DescriptorSetHandle                  m_SeedFloodSet;
    std::unique_ptr<rhi::IRHIPipelineState>   m_SeedFloodPSO;
    bool                                      m_SeedSetBound = false;
    std::unique_ptr<rhi::IRHIBuffer>          m_Positions;   // float4（w 未用）
    std::unique_ptr<rhi::IRHIBuffer>          m_Indices;
    std::unique_ptr<rhi::IRHIBuffer>          m_ProbeDist;   // CPU 可读（自检）
    u32 m_ProbeCount = 0;
    /// 自检探针针对哪个 mesh：取 **AABB 最大**的那个 —— 大网格是当前"注入值偏小"的头号怀疑对象，
    /// 只查 mesh 0（小网格）会把问题漏掉（实测：全局层在探针处为 0，而该点真实距离 894）
    u32 m_ProbeMeshIndex = 0;

    // 状态机
    enum class Phase { Idle, Baking, WaitSelfCheck, GlobalBuild, WaitGlobalCheck, March, WaitMarchCheck, Done };
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

    // ── Global SDF（步骤 10）──
    // ── Global SDF（步骤 10 / clipmap 分层）──
    /// 一层 clipmap：u32 原子目标 + R32F 可采样输出 + 自检探针
    struct GlobalLayer {
        std::unique_ptr<rhi::IRHITexture> scratch;
        std::unique_ptr<rhi::IRHITexture> field;
        std::unique_ptr<rhi::IRHIBuffer>  probe;
        float3 origin    = float3(0.0f);
        float  voxelSize = 0.0f;
        u32    res = 0, probeCount = 0;
        GlobalCheck check;   // 每层单独自检（近层紧度是本步的关键指标）
    };
    static constexpr u32 kMaxGlobalLayers = 2;
    rhi::DescriptorSetLayoutHandle m_GlobalLayout;
    rhi::DescriptorSetHandle       m_GlobalSet;
    // 【为什么每层/每 mesh 各一套描述符集】同一套描述符集在一次提交里被反复改写会出现
    // "所有 dispatch 都看到最后一次写入"（描述符集别名）——实测注入因此全部采到同一张（0 值）纹理。
    std::vector<rhi::DescriptorSetHandle> m_GlobalLayerSets;    // 每层一套（clear/flood/convert/probe）
    std::vector<rhi::DescriptorSetHandle> m_GlobalInjectSets;   // 每 层×mesh 一套（注入）
    std::unique_ptr<rhi::IRHIPipelineState> m_GlobalPSO;
    std::unique_ptr<rhi::IRHIPipelineState> m_GlobalFloodPSO;   // 全局网格上的跳步洪泛
    std::unique_ptr<rhi::IRHIPipelineState> m_LayerProbePSO;    // 独立取样（把层场写进探针缓冲）
    std::unique_ptr<rhi::IRHISampler>       m_NearestSampler;
    GlobalLayer m_GlobalLayers[kMaxGlobalLayers];
    u32 m_GlobalLayerCount = 0;
    float3 m_CameraPos = float3(0.0f);   // clipmap 近层的中心（跟随相机）
    bool   m_CameraPosSet = false;
    u32 m_WaitGlobalFrames = 0;

    // ── sphere tracing（步骤 11）──
    rhi::DescriptorSetLayoutHandle m_MarchLayout;
    rhi::DescriptorSetHandle       m_MarchSet;
    std::unique_ptr<rhi::IRHIPipelineState> m_MarchPSO;
    std::unique_ptr<rhi::IRHIBuffer> m_RayOrigin;
    std::unique_ptr<rhi::IRHIBuffer> m_RayDir;
    std::unique_ptr<rhi::IRHIBuffer> m_RayHit;      // CPU 可读
    std::unique_ptr<rhi::IRHIBuffer> m_RayNormal;   // CPU 可读
    // 细节追踪（逐 mesh）：min 归约的命中距离缓冲（u32 位模式，+inf = 未命中）
    rhi::DescriptorSetLayoutHandle m_DetailLayout;
    rhi::DescriptorSetHandle       m_DetailSet;
    std::unique_ptr<rhi::IRHIPipelineState> m_DetailPSO;
    std::unique_ptr<rhi::IRHIBuffer> m_RayT;
    void* m_RayTMapped = nullptr;
    std::unique_ptr<rhi::IRHISampler> m_LinearSampler;
    std::vector<float3> m_RayOriginCPU;
    std::vector<float3> m_RayDirCPU;
    u32 m_WaitMarchFrames = 0;
    MarchCheck m_MarchCheck;

    // ── SDF 追踪可视化（步骤 12）──
    rhi::DescriptorSetLayoutHandle m_DebugLayout;
    rhi::DescriptorSetHandle       m_DebugSet;
    std::unique_ptr<rhi::IRHIPipelineState> m_DebugPSO;
    std::unique_ptr<rhi::IRHITexture>       m_DebugTex;      // RGBA16F，屏幕尺寸
    std::unique_ptr<rhi::IRHIBuffer>        m_DebugStats;    // CPU 可读（原子计数）
    std::unique_ptr<rhi::IRHIBuffer>        m_DebugProbe;    // CPU 可读（沿中心射线的场剖面）
    void* m_DebugStatsMapped = nullptr;
    u32   m_ViewportW = 0, m_ViewportH = 0;
    float3 m_DebugCamPos = float3(0.0f);   // 最近一次调试视图的相机位置（诊断用）
    float3 m_DebugCamFwd = float3(0.0f, 0.0f, -1.0f);   // 最近一次的前向（剖面点用）
    u32   m_DebugFrames = 0;                                 // 已累计统计的帧数
    u32   m_DebugStatsLast[4] = {0, 0, 0, 0};
    // mesh 场探针（每帧查一个 mesh，读回等 3 帧）
    u32   m_MeshProbeIndex = 0;
    u32   m_MeshProbeStage = 0;      // 0 = 发射，1 = 等读回
    u32   m_MeshProbeFrame = 0;
    bool  m_MeshProbeReported = false;
    std::vector<float> m_MeshProbeValue;   // 每个 mesh 的命中距离（≈0 ⇒ 该 mesh 场在此处为 0）
};

} // namespace he::render
