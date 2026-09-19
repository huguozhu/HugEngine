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

private:
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
};

} // namespace he::render
