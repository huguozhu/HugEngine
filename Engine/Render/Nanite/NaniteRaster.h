#pragma once

// ============================================================
// Nanite/NaniteRaster.h — 软光栅（后续追加硬光栅分支）
//
// 【§14.8 任务 3：绘制端（消费计数）】
//   任务 1 只有生命周期桩；任务 3 起本类做出一条**极小**的绘制通道：
//     · 用新的 RHI 接口 `IRHICommandList::DrawIndexedIndirectCount` 消费
//       `NaniteCull` 写出的「计数缓冲 + 间接命令缓冲」——绘制条数由 GPU 决定；
//     · 渲染目标是**模块自建的 1×1 R8 小目标**（不是 GBuffer 的任何附件），
//       片元着色器每被光栅化一个簇就把"已光栅化簇数"原子加一。
//   因此本 pass 对可见画面零影响（§14.2 不变式 1）。
//
// 【任务 4 起】这里才会出现真正写 GBuffer 的软光栅；按 §14.5 的裁决（A1/A2）
//   建模块自己的 PSO/附件布局，直接写既有 GBuffer 纹理句柄。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法）；不得依赖 `MeshBatcher` 的运行时状态
//   —— 它只当**一次性输入**。
// ============================================================

#include "RHI/RHI.h"

#include <memory>

namespace he::render {

class NaniteRaster {
public:
    NaniteRaster() = default;
    ~NaniteRaster() = default;

    NaniteRaster(const NaniteRaster&) = delete;
    NaniteRaster& operator=(const NaniteRaster&) = delete;

    /// 建立绘制端自持资源：1×1 R8 目标 + 图形 PSO + 片元描述符集。
    /// @param rasterCountBuffer `NaniteCull` 自持的"已光栅化簇计数缓冲"（本类只引用，不持有）
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height,
                    rhi::IRHIBuffer* rasterCountBuffer);

    void Shutdown();
    void OnResize(u32 width, u32 height);

    [[nodiscard]] bool IsReady() const { return m_Device != nullptr && m_PSO != nullptr; }

    /// 录制 `Nanite_Raster` pass：
    ///   `DrawIndexedIndirectCount(indirectCmdBuffer, 0, countBuffer, 0, maxDrawCount, 20)`
    void RecordRasterPass(rhi::IRHICommandList* cmd,
                          rhi::IRHIBuffer* indirectCmdBuffer,
                          rhi::IRHIBuffer* countBuffer,
                          u32 maxDrawCount);

    /// 【§14.8 任务 4：UAV 自证通道】录制 `Nanite_TestWrite` pass。
    /// 用 `RWTexture2D<float4>`（`Nanite_TestWrite.comp.slang`）往**既有 GBuffer albedo**
    /// 写 8×8 棋盘，证明 "compute 写既有 GBuffer（A1）且同帧被 Lighting 读到"。
    ///
    /// 【懒初始化】PSO 与描述符集在**首次真正录制时**才建（`EnsureTestWriteResources`）：
    ///   `testWrite` 默认关闭，关闭档下这些资源一个都不会创建（§14.2 不变式 1：
    ///   关闭时不产生新的每帧 CPU 开销，也不多建任何 GPU 资源）。
    ///
    /// 【资源只借用不持有】`albedo` 是 `GBufferRenderer` 的纹理，本类只把它绑成存储图像，
    ///   不参与其生命周期；分辨率也从纹理自身取（`GetWidth/GetHeight`）。
    void RecordTestWritePass(rhi::IRHICommandList* cmd, rhi::IRHITexture* albedo);

    // ============================================================
    // 【§14.8 任务 6】mesh PSO 自证通道（最小 mesh 管线 + 非空输出读回）
    //
    // 【为什么放在本类】任务 6 的产物是"另一条绘制端点"（mesh 管线替代 VS+IA），
    //   与既有软/间接绘制端同属 `NaniteRaster` 的职责（§14.3：本文件是"软光栅（后加硬光栅分支）"），
    //   故不新开文件、不扩大模块公共面。
    //
    // 【懒初始化】与任务 4 的 UAV 自证通道同理：`meshTest` 默认关，关闭档下 mesh 目标/缓冲/PSO
    //   一个都不会创建（§14.2 不变式 1：关闭时不产生新的每帧 CPU 开销，也不多建任何 GPU 资源）。
    //
    // 【设备要求】需要 `VK_EXT_mesh_shader`（`DeviceCaps::supportsMeshShaders`）。
    //   不支持时**不建 PSO、不记录**，并由 `IsMeshTestSupported()` 让帧图侧连 pass 都不注册
    //   —— 不做任何替代方案（与任务 3 对 `DrawIndexedIndirectCount` 的处理口径一致）。
    // ============================================================

    /// 设备是否具备 mesh shader 能力（`Initialize` 时查一次 `DeviceCaps::supportsMeshShaders`）。
    [[nodiscard]] bool IsMeshTestSupported() const { return m_MeshShaderSupported; }

    /// mesh PSO 是否真的建起来了（dump 帧日志 `mesh_pso=ok/fail` 的依据）。
    [[nodiscard]] bool IsMeshTestPSOReady() const { return m_MeshTestPSO != nullptr; }

    /// 录制 `Nanite_MeshTest` pass：`DrawMeshTasks(1,1,1)` 画进模块自建的 1×1 R8 目标，
    /// 并把该目标拷进 host 可见缓冲供 dump 帧读回（拷贝在 render pass 之外录制）。
    void RecordMeshTestPass(rhi::IRHICommandList* cmd);

    /// 读回 mesh 通道的「被光栅化图元数」（片元原子计数缓冲的真实 GPU 读回）。
    /// 【同步约定】调用方必须保证 GPU 已完成（样例 dump 路径已有 `WaitIdle()`）。
    [[nodiscard]] u32 ReadbackMeshTestOutputs();

    /// 读回 mesh 通道 1×1 R8 目标像素的**最大**值（`CopyTextureToBuffer` → Map 的真实 GPU 读回）。
    [[nodiscard]] u32 ReadbackMeshTestTargetMax();

private:
    /// 懒建 `Nanite_TestWrite` 的 PSO + 描述符集布局（首次录制时调用一次）。
    /// 返回 false 表示创建失败（调用方跳过本次录制，不影响其它 pass）。
    bool EnsureTestWriteResources();

    /// 懒建 `Nanite_MeshTest` 的目标/缓冲/描述符集/mesh PSO（首次录制时调用一次）。
    /// 返回 false 表示创建失败或设备不支持 mesh shader。
    bool EnsureMeshTestResources();

    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width  = 0;
    u32 m_Height = 0;

    /// 模块自建的小目标（R8，1×1）。它**不在** GBuffer 里，写它不会改变可见画面。
    std::unique_ptr<rhi::IRHITexture> m_Target;
    /// 绘制端不读顶点属性，但 `DrawIndexedIndirectCount` 仍要求绑定索引/顶点缓冲
    std::unique_ptr<rhi::IRHIBuffer>  m_DummyVB;
    std::unique_ptr<rhi::IRHIBuffer>  m_DummyIB;

    rhi::ShaderBytecode m_VS;   // Nanite_Raster.vert.spv
    rhi::ShaderBytecode m_FS;   // Nanite_Raster.frag.spv

    rhi::DescriptorSetLayoutHandle m_Layout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_Set    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIPipelineState> m_PSO;

    /// 最近一次 pass 传入的 maxDrawCount（诊断用）
    u32 m_LastMaxDrawCount = 0;

    // ── §14.8 任务 4：UAV 自证通道（懒建；testWrite 关闭时全部为空）──
    rhi::ShaderBytecode            m_TestWriteCS;                                  // Nanite_TestWrite.comp.spv
    rhi::DescriptorSetLayoutHandle m_TestWriteLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_TestWriteSet    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIPipelineState> m_TestWritePSO;

    // ── §14.8 任务 6：mesh PSO 自证通道（懒建；meshTest 关闭时全部为空）──
    /// 设备能力（`Initialize` 时查询一次；false ⇒ 永不建 mesh PSO，也不注册 pass）
    bool m_MeshShaderSupported = false;

    rhi::ShaderBytecode m_MeshTestMS;   // Nanite_MeshTest.mesh.spv
    rhi::ShaderBytecode m_MeshTestFS;   // Nanite_MeshTest.frag.spv

    /// 模块自建的 1×1 R8 小目标：**不是** GBuffer 的任何附件 ⇒ 改不动可见画面。
    /// usage 比任务 3 的目标多 `TransferSrc`（任务 6 要把它读回 host）与 `ShaderResource`
    /// （`CopyTextureToBuffer` 拷完无条件还原成 SHADER_READ_ONLY；缺 SAMPLED 位会报
    /// VUID-VkImageMemoryBarrier-oldLayout-01211）。
    std::unique_ptr<rhi::IRHITexture> m_MeshTestTarget;
    /// 片元原子计数：本帧 mesh shader 真正输出并被光栅化的图元数（dump 帧读回）
    std::unique_ptr<rhi::IRHIBuffer>  m_MeshTestCount;
    /// 1×1 R8 目标 → host 的读回缓冲（dump 帧 Map；照仓库既有 buffer 读回写法）
    std::unique_ptr<rhi::IRHIBuffer>  m_MeshTestReadback;

    rhi::DescriptorSetLayoutHandle m_MeshTestLayout = rhi::kInvalidLayout;
    rhi::DescriptorSetHandle       m_MeshTestSet    = rhi::kInvalidSet;
    std::unique_ptr<rhi::IRHIPipelineState> m_MeshTestPSO;
};

} // namespace he::render
