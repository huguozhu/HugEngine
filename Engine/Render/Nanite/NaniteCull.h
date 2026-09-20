#pragma once

// ============================================================
// Nanite/NaniteCull.h — 实例剔除 + cluster BVH 剔除 + Hi-Z 遮挡
//
// 【§14.8 任务 3：模块自持的「计数 → 间接绘制」链】
//   任务 1/2 只有生命周期桩；任务 3 起本类自持**四个缓冲**并把"计数"端做出来：
//     · 假簇输入缓冲      —— N 条 `NaniteFakeCluster`（1 个实例、N 个簇）
//     · 间接命令缓冲      —— N 条 `NaniteIndirectCommand`（GPU 压缩写入）
//     · 计数缓冲          —— 单个 u32（GPU 原子累加"实际写入的命令条数"）
//     · 光栅化簇计数缓冲  —— 单个 u32（绘制端每光栅化一个簇原子加一）
//   compute 写命令与计数后插入一次 `ComputeShader → DrawIndirect` 屏障，
//   绘制端（`NaniteRaster`）用 `DrawIndexedIndirectCount` 直接消费该计数。
//   **不改 `GPUCulling`**：模块自持整条链（§14.5 末段）。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法 —— 本类将来接收 Hi-Z 纹理句柄即可，
//   不去 include `GPUCulling.h` 的私有成员）；不得依赖 `MeshBatcher` 的运行时状态
//   —— 它只当**一次性输入**。
//
// 【§14.8 任务 13：实例剔除通道】在原有四条链路之外，本类再自持一组"实例剔除"资源：
//     · 实例缓冲        —— N 条 128B 的 `NaniteInstanceGpuObject`（= GPUSceneObject 契约）
//     · 包围球缓冲      —— N 条 16B 的 `NaniteInstanceSphere`（CPU 从 128B 的 bounds 推一次）
//     · 可见实例列表    —— N 个 u32（GPU 原子压缩写入；CPU 参考剔除给出升序基准）
//     · 可见实例计数    —— 单个 u32（GPU 原子累加；**每帧在命令缓冲内清 0**，见 `RecordInstanceCullPass`）
//     · 计数清零源      —— 单个 u32 常驻 0（TransferSrc；上面那次"清 0"的拷贝源）
//   与任务 3 的假簇链**互相独立**（各自一套缓冲/描述符/PSO），互不影响既有验收读数。
//   【列表不需要逐帧重置】读回只取 `[0, 计数)`，而这些槽位必定由**同一次派发**写入（计数与列表
//   写在同一个着色器里）；不重置反而消掉了"主机 memset 与派发竞争"的隐患。
//
// 【§14.8 任务 14：per-instance cluster BVH 遍历】本类再自持一组"簇 BVH"资源：
//     · 节点缓冲        —— `NaniteBVHNode`（32B/条；CPU 构建器产出，一次性上传）
//     · 叶子簇表缓冲    —— u32（叶子用 [left, left+count)）
//     · 簇球缓冲        —— `NaniteClusterSphere`（16B/条；CPU 从簇记录逐位搬运）
//     · 可见簇列表      —— `NaniteVisibleClusterRef`（8B/条；GPU 原子压缩写）
//     · 可见簇计数      —— 单个 u32（GPU 原子累加）
//     · 已访问节点计数  —— 单个 u32（GPU 原子累加；验收的"遍历访问数"）
//     · 清零源          —— 8B 常驻 0（TransferSrc；上面两个计数每帧的"清 0"拷贝源）
//   两个计数**每帧在命令缓冲内用 4B 拷贝清 0**（照任务 13 修法，不用主机写：见
//   `RecordInstanceCullPass` 里那段负向验证）；可见簇列表**不逐帧重置**，理由与任务 13 相同。
//
//   【Phase 1 → Phase 2 的接线为什么留到任务 15（如实说明）】Phase 2 的形式是"对每个**可见**实例
//   遍历 BVH"，但 `Nanite_InstanceCull` 与本 pass 在帧图里都**不声明任何帧图资源**，
//   `RenderGraph::TopologicalSort` 对 inDegree=0 的 pass 按 LIFO 处理 ⇒ **帧图无法表达**
//   "本 pass 必须排在实例剔除之后"这条顺序（同 Task 3 的假簇链）。本任务因此把 Phase 2 的
//   实例域定义为"**全部非空实例**"（`indexCount != 0`，与任务 13 的跳过规则同一个判据），
//   这是一个**自洽、确定、CPU/GPU 同口径**的域。真正的三阶段接线（Phase 1 可见列表 → Phase 2
//   → Hi-Z → Phase 3 LOD 选择）正是 §14.8 任务 15 的正文，届时两个 pass 必须合并进同一条链
//   （或让实例剔除把结果落到帧图资源上）—— 已写进任务 15 的记录项。
// ============================================================

#include "Nanite/NaniteTypes.h"
#include "Nanite/NaniteUpload.h"   // 【任务 14】`NaniteClusterBVH`（构建产物；RHI-free 头）
#include "RHI/RHI.h"
#include "Math/Math.h"   // 【任务 13】float3 / float4x4（相机视锥与合成实例网格的输入类型）

#include <memory>
#include <vector>
#include <span>   // 【任务 14】SetClusterBVH 的簇记录视图

namespace he::render {

/// Nanite_Cull.comp.slang 的 push constant（逐字段对应；static_assert 钉住 8 字节）
struct alignas(4) NaniteCullParams {
    u32 clusterCount;           // 本帧假簇数量
    u32 vertexCountPerCluster;  // 每条间接命令的 indexCount（假数据 = 3）
};
static_assert(sizeof(NaniteCullParams) == 8, "NaniteCullParams 必须与 Slang cbuffer 一致（2×u32）");

/// 【§14.8 任务 13】Nanite_InstanceCull.comp.slang 的 push constant
///
/// 【为什么用 push constant 传视锥】6 个平面每帧只变一次（相机），且远小于 128B 的下限；
///   走小块 push constant 不必再建 UBO/描述符，也不会与既有 pass 的描述符集打架。
/// 【布局】`planes[6]`（std430 下 float4 步长 16 ⇒ 96B）+ 4 个 u32（16B）= 112B；
///   与 Slang 侧的 `NaniteInstanceCullParams` 逐字段一致。
struct alignas(16) NaniteInstanceCullParams {
    float planes[6][4];   // 偏移 0：世界空间视锥六平面（[左,右,下,上,近,远]，n 已归一化）
    u32   instanceCount;  // 偏移 96：本帧实例数
    u32   _pad0;          // 偏移 100
    u32   _pad1;          // 偏移 104
    u32   _pad2;          // 偏移 108
};
static_assert(sizeof(NaniteInstanceCullParams) == 112,
              "NaniteInstanceCullParams 必须与 Slang cbuffer 一致（6×float4 + 4×u32 = 112B）");
static_assert(offsetof(NaniteInstanceCullParams, instanceCount) == 96,
              "instanceCount 必须紧跟 6 个 float4（偏移 96）");

/// 【§14.8 任务 14】Nanite_ClusterBVH.comp.slang 的 push constant
///
/// 【布局】`planes[6]`（96B）+ 4 个 u32（16B）= 112B，与任务 13 的实例剔除同形（同一套视锥提取）。
///   `visibleCapacity` 进 push constant 是为了让 shader 自己判断"槽位是否越出可见列表容量"，
///   从而 GPU 的写入与 CPU 参考的写入口径（只写容量内、计数照常累加）逐条一致。
struct alignas(16) NaniteClusterBVHParams {
    float planes[6][4];     // 偏移 0：世界空间视锥六平面（[左,右,下,上,近,远]，n 已归一化）
    u32   instanceCount;    // 偏移 96：本帧实例数（合成实例表的条数）
    u32   clusterCount;     // 偏移 100：簇数（= 簇球表条数）
    u32   nodeCount;        // 偏移 104：BVH 节点数
    u32   visibleCapacity;  // 偏移 108：可见簇列表容量
};
static_assert(sizeof(NaniteClusterBVHParams) == 112,
              "NaniteClusterBVHParams 必须与 Slang cbuffer 一致（6×float4 + 4×u32 = 112B）");
static_assert(offsetof(NaniteClusterBVHParams, instanceCount) == 96,
              "instanceCount 必须紧跟 6 个 float4（偏移 96）");
static_assert(offsetof(NaniteClusterBVHParams, visibleCapacity) == 108,
              "visibleCapacity 必须在偏移 108");

class NaniteCull {
public:
    NaniteCull() = default;
    ~NaniteCull() = default;

    NaniteCull(const NaniteCull&) = delete;
    NaniteCull& operator=(const NaniteCull&) = delete;

    /// 建立"计数 → 间接绘制"链的自持资源：四个缓冲 + compute PSO + 描述符集。
    /// @param rasterCountBuffer 由本类创建并持有的"已光栅化簇计数缓冲"（绘制端引用它）
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);

    /// 释放自持资源（缓冲/PSO/描述符集布局）
    void Shutdown();
    void OnResize(u32 width, u32 height);

    [[nodiscard]] bool IsReady() const {
        return m_Device != nullptr && m_PSO != nullptr && m_InstanceCullPSO != nullptr
            && m_BVHPSO != nullptr;   // 【任务 14】BVH 遍历的 compute 管线也必须建成
    }

    /// 设置本帧假簇数量（超上限钳制）。由 `NaniteRenderer::AddPasses` 从
    /// `NaniteSettings::fakeClusters` 转发，是任务 3 的唯一输入。
    void SetFakeClusterCount(u32 count);
    [[nodiscard]] u32 GetFakeClusterCount() const { return m_FakeClusterCount; }

    // ============================================================
    // §14.8 任务 13：实例剔除（视锥 → 可见实例列表 + 计数）
    // ============================================================

    /// 设置本帧实例剔除的输入：相机（世界空间视锥）与**合成实例网格**条数。
    ///
    /// 【实例来源与坐标系（如实说明：它们**不是**场景实例）】本任务还没有"场景 → 模块实例表"
    ///   的接入点（属任务 14+），验收要的是"GPU 与 CPU 参考逐项一致"这个**可判定的等价性**，
    ///   因此这里用一张**合成**实例网格：
    ///     · 在 **NDC（view-projection 空间）** 上摆一个 `nx × ny` 网格（整体外扩 1.25 倍 ⇒
    ///       边缘必然落到视锥外），再经 `inverse(viewProj)` **反投影到世界空间**；
    ///     · 另取几个下标**故意**摆到视锥外（NDC 2.5）、**故意跨越**右侧平面（NDC 恰好 1.0 +
    ///       足够的半径）、以及一个 `indexCount = 0` 的空实例（验证"空实例跳过"规则）；
    ///     · 每实例的 128B 条目按 `GPUSceneObject` 契约填：`localToWorld` = 平移、`boundsMin/Max`
    ///       = 球心 ± 半径、`indexCount` 非 0；包围球再由 `NaniteSphereFromInstanceBounds`
    ///       从 `boundsMin/Max` 推出 ⇒ **球确实来自 128B 契约**，而不是另一个来源。
    ///   · 网格随相机走（用 `cameraPosition` 与 `viewProj` 反投影），因此相机移动时样本集合
    ///     依然覆盖"里/外/跨越"三类，不依赖硬编码的世界坐标。
    /// 【调用时机】`NaniteRenderer::AddPasses` 每帧调用一次（帧图构建期）。
    void SetInstanceCullFrame(const float4x4& viewProj, const float3& cameraPosition,
                              u32 testInstanceCount);

    /// 录制 `Nanite_InstanceCull` pass：
    ///   ① 上传合成实例表 + 包围球（主机可见缓冲，与任务 3 同款）；
    ///   ② CPU 参考剔除（`NaniteCullInstancesCPU`）并保存结果，供 dump 帧逐项比较；
    ///   ③ **命令缓冲内**把可见计数清 0（4B 拷贝，GPU 有序）+ `Transfer → Compute` 屏障；
    ///   ④ Dispatch（每实例一个线程）+ `Compute → Compute|Transfer` 屏障（后者为下一帧的清零消 WAR）。
    /// 【为什么清零必须进命令缓冲】录制期的主机写会与派发竞争（CPU 领先 GPU ⇒ 两帧原子累加叠加，
    ///   实测读回恰为 CPU 参考的 2 倍）。完整负向验证见 `NaniteCull.cpp` 的 `RecordInstanceCullPass`。
    void RecordInstanceCullPass(rhi::IRHICommandList* cmd);

    // ── 实例剔除的读回访问（模块内部与 `NaniteRenderer::LogInstanceCullReadback` 使用）──
    [[nodiscard]] rhi::IRHIBuffer* GetVisibleInstanceBuffer()      const { return m_VisibleInstanceBuf.get(); }
    [[nodiscard]] rhi::IRHIBuffer* GetVisibleInstanceCountBuffer() const { return m_VisibleInstanceCountBuf.get(); }
    /// CPU 参考剔除的可见实例下标（升序）—— 最近一次 `RecordInstanceCullPass` 的结果
    [[nodiscard]] const std::vector<u32>& GetCpuVisibleInstances() const { return m_CpuVisibleInstances; }
    /// 本帧合成实例条数（= 最近一次 `SetInstanceCullFrame` 的钳制后入参）
    [[nodiscard]] u32 GetTestInstanceCount() const { return m_TestInstanceCount; }
    [[nodiscard]] static constexpr u32 MaxTestInstances() { return kNaniteMaxTestInstances; }

    // ============================================================
    // §14.8 任务 14：per-instance cluster BVH（构建产物入库 + 每帧深度优先遍历）
    // ============================================================

    /// 按 `.nanite` 的簇记录构建 BVH 并**一次性上传**三个只读缓冲（节点 / 叶子簇表 / 簇球）。
    ///
    /// 【调用时机与次数】`NaniteRenderer::EnsureAssetUploaded` 在开关开启时**只调一次**
    ///   （资产构建成功后）。之后每帧不再碰这三个缓冲。
    /// 【簇数上限】按 `kNaniteMaxBVHClusters` 截断（超出时打印一次中文告警，不静默）；
    ///   被截断掉的是"下标 ≥ 上限"的簇 —— 可见簇引用表的大小由这个上限推出。
    /// 【失败】设备/PSO 未就绪、构建失败 ⇒ 返回 false（此后该 pass 直接跳过，不派发）。
    /// 【同步约定】本函数只在**一次性启动路径**上被调用（与任务 12 的资产上传同一时机），
    ///   此缓冲尚未被任何已提交的 GPU 工作引用 ⇒ 主机 `Map` 写入不存在竞争。
    [[nodiscard]] bool SetClusterBVH(std::span<const NaniteClusterRecord> clusters);

    /// 本 pass 是否可用（BVH 已入库 + PSO/描述符集就绪）
    [[nodiscard]] bool IsClusterBVHReady() const { return m_BVHReady; }

    /// 录制 `Nanite_ClusterBVH` pass：
    ///   ① **命令缓冲内**把可见簇计数与已访问节点计数清 0（两次 4B 拷贝，GPU 有序）+ 屏障；
    ///   ② Dispatch（每实例一个线程；实例域 = `min(合成实例数, kNaniteMaxBVHInstances)`）；
    ///   ③ 屏障（compute → compute|transfer，后者为下一帧的清零消 WAR）。
    /// 【为什么与实例剔除分成两个 pass】本任务只做"构建 + 遍历"；三阶段合并属任务 15。
    void RecordClusterBVHPass(rhi::IRHICommandList* cmd);

    /// 【任务 14】CPU 参考遍历（dump 帧算一次；输入与 GPU **同一份比特**：同一个视锥、同一张
    ///   128B 实例表、同一棵 BVH、同一张簇球表、同一个实例域钳制）。
    /// @param outVisible 输出可见簇引用（会被 resize 到可见数）
    /// @return 遍历读数（visited / visible / stackOverflows / traversedInstances）
    NaniteClusterBVHTraversalStats RunClusterBVHCPUReference(
        std::vector<NaniteVisibleClusterRef>& outVisible) const;

    // ── 任务 14 的读回访问（`NaniteRenderer::LogClusterBVHReadback` 使用）──
    [[nodiscard]] rhi::IRHIBuffer* GetVisibleClusterBuffer()      const { return m_VisibleClusterBuf.get(); }
    [[nodiscard]] rhi::IRHIBuffer* GetVisibleClusterCountBuffer() const { return m_VisibleClusterCountBuf.get(); }
    [[nodiscard]] rhi::IRHIBuffer* GetBVHVisitedCountBuffer()     const { return m_BVHVisitedCountBuf.get(); }
    [[nodiscard]] u32 GetBVHNodeCount()    const { return (u32)m_BVHData.nodes.size(); }
    [[nodiscard]] u32 GetBVHDepth()        const { return m_BVHData.depth; }
    [[nodiscard]] u32 GetBVHClusterCount() const { return m_BVHData.clusterCount; }
    [[nodiscard]] u32 GetBVHInstanceDomain() const { return m_BVHInstanceDomain; }
    [[nodiscard]] u32 GetBVHVisibleCapacity() const { return kNaniteMaxVisibleClusterRefs; }
    [[nodiscard]] static constexpr u32 MaxBVHInstances() { return kNaniteMaxBVHInstances; }

    /// 录制 `Nanite_Cull` pass：
    ///   ① CPU 侧每帧重置三个计数/命令缓冲（沿用引擎既有的 `Map` 清零写法）
    ///   ② 上传 N 条假簇
    ///   ③ Dispatch（每簇一个线程）
    ///   ④ 插入 `ComputeShader → DrawIndirect` 屏障
    void RecordCullPass(rhi::IRHICommandList* cmd);

    // ── 绘制端 / 读回所需的缓冲访问（模块内部使用，外部不得越过 NaniteRenderer）──
    [[nodiscard]] rhi::IRHIBuffer* GetFakeClusterBuffer()  const { return m_FakeClusterBuf.get(); }
    [[nodiscard]] rhi::IRHIBuffer* GetIndirectCmdBuffer()  const { return m_IndirectCmdBuf.get(); }
    [[nodiscard]] rhi::IRHIBuffer* GetCountBuffer()        const { return m_CountBuf.get(); }
    [[nodiscard]] rhi::IRHIBuffer* GetRasterCountBuffer()  const { return m_RasterCountBuf.get(); }
    [[nodiscard]] u32 GetMaxFakeClusters() const { return kNaniteMaxFakeClusters; }

private:
    /// 每帧重置：计数缓冲清零 / 间接命令缓冲填哨兵 / 光栅化簇计数清零
    /// （CPU 侧 Map 写入；与 `GPUCulling::DispatchPhase2` 的清零写法一致，不发明新同步机制）
    void ResetFrameBuffers();
    /// 把 N 条假簇写进输入缓冲（N 变化或首帧时才需要，但每帧写一遍成本可忽略）
    void UploadFakeClusters();

    /// 【任务 13】实例剔除缓冲的**启动初值**（`Initialize` 调一次）：计数清零 + 列表填哨兵。
    /// 【不要**每帧**调它】每帧的计数清零是命令缓冲里的 4B 拷贝（GPU 有序）——录制期的主机写
    ///   会与派发竞争（CPU 领先 GPU ⇒ 两帧原子累加叠加，实测读回恰为 CPU 参考的 2 倍），
    ///   完整实测与根因见 `NaniteCull.cpp` 的 `RecordInstanceCullPass`。
    void ResetInstanceCullBuffers();
    /// 【任务 13】按 NDC 网格 + 反投影生成合成实例表与包围球（口径见 `SetInstanceCullFrame`）
    void BuildTestInstances();
    /// 【任务 13】把合成实例表与包围球写进各自的 GPU 缓冲
    void UploadInstanceCullInputs();
    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width  = 0;
    u32 m_Height = 0;

    /// 本帧假簇数量（任务 3 的输入；默认与 `NaniteSettings::fakeClusters` 一致）
    u32 m_FakeClusterCount = 6;

    // ── 任务 3 自持的四个缓冲 ──
    std::unique_ptr<rhi::IRHIBuffer> m_FakeClusterBuf;  // 假簇输入（Storage，CPU 每帧写）
    std::unique_ptr<rhi::IRHIBuffer> m_IndirectCmdBuf;  // 间接命令（Storage|Indirect，GPU 写）
    std::unique_ptr<rhi::IRHIBuffer> m_CountBuf;        // 命令条数（Storage|Indirect，GPU 原子写）
    std::unique_ptr<rhi::IRHIBuffer> m_RasterCountBuf;  // 已光栅化簇数（Storage，片元原子写）

    // ── compute 管线 ──
    rhi::ShaderBytecode m_CS;   // Nanite_Cull.comp.spv
    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;

    // ============================================================
    // §14.8 任务 13：实例剔除自持资源与状态
    // ============================================================

    /// 本帧合成实例条数（`SetInstanceCullFrame` 的钳制后入参；默认 0 = 尚未设置）
    u32 m_TestInstanceCount = 0;

    // ── 四个自持缓冲（容量都是 `kNaniteMaxTestInstances`）──
    std::unique_ptr<rhi::IRHIBuffer> m_InstanceBuf;             // 128B 实例表（Storage，CPU 每帧写）
    std::unique_ptr<rhi::IRHIBuffer> m_InstanceSphereBuf;       // 16B 包围球（Storage，CPU 每帧写）
    std::unique_ptr<rhi::IRHIBuffer> m_VisibleInstanceBuf;      // 可见实例列表（Storage，GPU 原子压缩写）
    std::unique_ptr<rhi::IRHIBuffer> m_VisibleInstanceCountBuf; // 可见实例计数（Storage|TransferDst，GPU 原子写）
    /// 【任务 13】计数清零的**源**缓冲（常驻 0，TransferSrc）：
    ///   每帧开头的"计数清零"是命令缓冲内的 4B 拷贝（GPU 有序），而不是录制期的主机写。
    std::unique_ptr<rhi::IRHIBuffer> m_VisibleCountClearBuf;

    // ── 实例剔除 compute 管线 ──
    rhi::ShaderBytecode m_InstanceCullCS;   // Nanite_InstanceCull.comp.spv
    rhi::DescriptorSetLayoutHandle m_InstanceCullLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_InstanceCullSet    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIPipelineState> m_InstanceCullPSO;

    // ── 帧状态（CPU 侧；`RecordInstanceCullPass` 用它们做参考剔除）──
    NaniteFrustumPlanes m_FrameFrustum{};      // 本帧视锥（由 viewProj 提取）
    float4x4            m_FrameViewProj{1.0f}; // 本帧 view-proj（反投影合成实例网格用）
    float3              m_FrameCameraPos{0.0f};// 本帧相机世界坐标（网格深度/半径基准）
    NaniteInstanceCullParams m_InstanceCullParams{};  // 最近一次 push constant（含 planes + count）

    // 合成实例表与包围球（CPU 侧镜像；每帧按 `m_TestInstanceCount` 重建/上传）
    std::vector<NaniteInstanceGpuObject> m_TestInstances;
    std::vector<NaniteInstanceSphere>    m_TestSpheres;
    // CPU 参考剔除结果（升序可见下标）—— dump 帧与 GPU 读回逐项比较
    std::vector<u32> m_CpuVisibleInstances;

    // ============================================================
    // §14.8 任务 14：per-instance cluster BVH 的自持资源与状态
    // ============================================================

    /// BVH 是否已入库（`SetClusterBVH` 成功）。未入库 ⇒ `RecordClusterBVHPass` 直接跳过。
    bool m_BVHReady = false;

    /// CPU 侧 BVH 镜像（节点/叶子簇表/簇球/读数）——GPU 侧三个只读缓冲由它上传；
    /// 同时是 CPU 参考遍历的输入 ⇒ **GPU 与 CPU 读的是同一份比特**。
    NaniteClusterBVH m_BVHData;

    /// 本帧实例域 = `min(合成实例数, kNaniteMaxBVHInstances)`（CPU 参考与 GPU 同口径）
    u32 m_BVHInstanceDomain = 0u;

    // ── 三个只读输入缓冲（容量固定，`SetClusterBVH` 一次性上传）──
    std::unique_ptr<rhi::IRHIBuffer> m_BVHNodeBuf;     // 节点（32B/条，容量 kNaniteMaxBVHNodes）
    std::unique_ptr<rhi::IRHIBuffer> m_BVHLeafBuf;     // 叶子簇表（u32/条，容量 kNaniteMaxBVHClusters）
    std::unique_ptr<rhi::IRHIBuffer> m_BVHSphereBuf;   // 簇球（16B/条，容量 kNaniteMaxBVHClusters）
    // ── 每帧由 GPU 写的可写缓冲 ──
    std::unique_ptr<rhi::IRHIBuffer> m_VisibleClusterBuf;       // 可见簇引用（8B/条，容量 kNaniteMaxVisibleClusterRefs）
    std::unique_ptr<rhi::IRHIBuffer> m_VisibleClusterCountBuf;  // 可见簇计数（u32）
    std::unique_ptr<rhi::IRHIBuffer> m_BVHVisitedCountBuf;      // 已访问节点计数（u32）
    /// 两个计数的清零源（8B 常驻 0，TransferSrc）：前 4B 给可见簇计数、后 4B 给访问计数。
    /// 与任务 13 的 `m_VisibleCountClearBuf` 同款；**每帧的清零在命令缓冲内**（不用主机写）。
    std::unique_ptr<rhi::IRHIBuffer> m_BVHZeroClearBuf;

    // ── cluster BVH 遍历的 compute 管线 ──
    rhi::ShaderBytecode m_BVHCS;   // Nanite_ClusterBVH.comp.spv
    rhi::DescriptorSetLayoutHandle m_BVHLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_BVHSet    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIPipelineState> m_BVHPSO;
};

} // namespace he::render
