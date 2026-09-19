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
// ============================================================

#include "Nanite/NaniteTypes.h"
#include "RHI/RHI.h"

#include <memory>

namespace he::render {

/// Nanite_Cull.comp.slang 的 push constant（逐字段对应；static_assert 钉住 8 字节）
struct alignas(4) NaniteCullParams {
    u32 clusterCount;           // 本帧假簇数量
    u32 vertexCountPerCluster;  // 每条间接命令的 indexCount（假数据 = 3）
};
static_assert(sizeof(NaniteCullParams) == 8, "NaniteCullParams 必须与 Slang cbuffer 一致（2×u32）");

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

    [[nodiscard]] bool IsReady() const { return m_Device != nullptr && m_PSO != nullptr; }

    /// 设置本帧假簇数量（超上限钳制）。由 `NaniteRenderer::AddPasses` 从
    /// `NaniteSettings::fakeClusters` 转发，是任务 3 的唯一输入。
    void SetFakeClusterCount(u32 count);
    [[nodiscard]] u32 GetFakeClusterCount() const { return m_FakeClusterCount; }

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
};

} // namespace he::render
