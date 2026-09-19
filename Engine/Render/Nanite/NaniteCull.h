#pragma once

// ============================================================
// Nanite/NaniteCull.h — 实例剔除 + cluster BVH 剔除 + Hi-Z 遮挡
//
// 【本文件由 §14.8 任务 1 建立骨架，内容由任务 3 起填充】
//   任务 1 只有生命周期桩：Initialize / Shutdown / OnResize / IsReady，
//   **不派发任何 compute、不创建任何缓冲**。
//   任务 3 起在这里做：模块自持的「计数缓冲 → IndirectCmdBuf」链
//   （绘制端用 `DrawIndexedIndirectCount`，**不改** `GPUCulling`，见 §14.5 末段）。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法 —— 本类将来接收 Hi-Z 纹理句柄即可，
//   不去 include `GPUCulling.h` 的私有成员）；不得依赖 `MeshBatcher` 的运行时状态
//   —— 它只当**一次性输入**。
// ============================================================

#include "RHI/RHI.h"

namespace he::render {

class NaniteCull {
public:
    NaniteCull() = default;
    ~NaniteCull() = default;

    NaniteCull(const NaniteCull&) = delete;
    NaniteCull& operator=(const NaniteCull&) = delete;

    /// 骨架就绪：只记住设备与视口尺寸。任务 3 起在这里建计数/间接命令缓冲与 compute PSO
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
