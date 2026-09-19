#pragma once

// ============================================================
// Nanite/NaniteUpload.h — .nanite 资产 + 合并几何 → GPU 缓冲
//
// 【本文件由 §14.8 任务 1 建立骨架，内容由任务 12 填充】
//   任务 1 只有生命周期桩：Initialize / Shutdown / OnResize / IsReady，
//   **不加载任何资产、不创建任何 GPU 缓冲**。
//   任务 12 起在这里做：`.nanite` 资产读取 + 从 `MeshBatcher` 的合并几何
//   **读一次**（只当一次性输入，不得依赖其运行时状态）→ 上传 GPU 缓冲。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法）；不得依赖 `MeshBatcher` 的运行时状态
//   —— 它只当**一次性输入**：本类的接口将来只接收"几何快照"式参数，
//   不持有 `MeshBatcher*` 去每帧回读它的内部表。
// ============================================================

#include "RHI/RHI.h"

namespace he::render {

class NaniteUpload {
public:
    NaniteUpload() = default;
    ~NaniteUpload() = default;

    NaniteUpload(const NaniteUpload&) = delete;
    NaniteUpload& operator=(const NaniteUpload&) = delete;

    /// 骨架就绪：只记住设备与视口尺寸。任务 12 起在这里建上传用的暂存/目标缓冲
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
