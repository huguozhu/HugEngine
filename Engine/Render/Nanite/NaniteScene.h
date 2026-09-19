#pragma once

// ============================================================
// Nanite/NaniteScene.h — Nanite 的数据宿主（实例表 / cluster 表 / 几何量化缓冲 / LOD 错误）
//
// 【本文件由 §14.8 任务 1 建立骨架，内容由后续任务填充】
//   任务 1 只有生命周期桩：Initialize / Shutdown / OnResize / IsReady。
//   **不创建任何 GPU 资源、不参与渲染**。后续在这里追加：
//     任务 3  模块自持的"计数 → 间接绘制"链所需缓冲（§14.8 任务 3）
//     任务 5  Nanite 段的实例表（分区契约见 `NaniteTypes.h`）
//     任务 12 合并几何/量化缓冲与 LOD 错误的 GPU 侧宿主
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法，属于后续任务的 .cpp 细节）；
//   不得依赖 `MeshBatcher` 的运行时状态 —— 它只当**一次性输入**（合并几何）。
//   本类尤其不得反向依赖任何 GI/Lumen 类型：它是"数据放在哪"的答案，不是"GI 怎么算"。
// ============================================================

#include "RHI/RHI.h"

namespace he::render {

class NaniteScene {
public:
    NaniteScene() = default;
    ~NaniteScene() = default;

    NaniteScene(const NaniteScene&) = delete;
    NaniteScene& operator=(const NaniteScene&) = delete;

    /// 骨架就绪：只记住设备与视口尺寸。任务 12 起在这里建实例表/cluster 表/几何缓冲
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);

    /// 释放自持资源。任务 1 无资源可释放，只清指针（**不动开关真值**）
    void Shutdown();

    /// 视口变化：只影响与屏幕尺寸相关的资源（世界空间资源不在此重建）
    void OnResize(u32 width, u32 height);

    [[nodiscard]] bool IsReady() const { return m_Device != nullptr; }

private:
    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width  = 0;
    u32 m_Height = 0;
};

} // namespace he::render
