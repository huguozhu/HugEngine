#pragma once

// ============================================================
// Nanite/NaniteRaster.h — 软光栅（后续追加硬光栅分支）
//
// 【本文件由 §14.8 任务 1 建立骨架，内容由任务 4 起填充】
//   任务 1 只有生命周期桩：Initialize / Shutdown / OnResize / IsReady，
//   **不建 PSO、不写任何 GBuffer 纹理**。
//   任务 4 起在这里做：按 §14.5 的裁决（A1：给 GBuffer 纹理加 UAV，软光栅用
//   `RWTexture2D` 写颜色 + 手动写深度；A2：模块自建 VisBuffer + 材质解析 pass）
//   建模块**自己**的 PSO/附件布局，直接写既有 GBuffer 纹理句柄 ——
//   因此不需要给 `GBufferRenderer` 加 `Mode::Nanite`。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法）；不得依赖 `MeshBatcher` 的运行时状态
//   —— 它只当**一次性输入**。
// ============================================================

#include "RHI/RHI.h"

namespace he::render {

class NaniteRaster {
public:
    NaniteRaster() = default;
    ~NaniteRaster() = default;

    NaniteRaster(const NaniteRaster&) = delete;
    NaniteRaster& operator=(const NaniteRaster&) = delete;

    /// 骨架就绪：只记住设备与视口尺寸。任务 4 起在这里建软光栅 PSO 与描述符集
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);

    void Shutdown();
    void OnResize(u32 width, u32 height);

    [[nodiscard]] bool IsReady() const { return m_Device != nullptr; }

private:
    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width  = 0;
    u32 m_Height = 0;
};

} // namespace he::render
