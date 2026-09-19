#pragma once

// ============================================================
// Lumen/LumenScene.h — Lumen 的持久资源宿主
//
// 【为什么要单独一个类】Lumen 的数据（Surface Cache atlas、Global SDF clipmap、探针缓冲）
// 都是**跨帧持久**的 GPU 资源，它们既不能走 RenderGraph 的瞬态分配 —— `rg.CreateTexture`
// 建出来的纹理 pass 拿不到 `IRHITexture*`（`RenderGraph.h:106-119` 没有由句柄取指针的接口，
// `PassExecuteFunc` 只收 `IRHICommandList*`）—— 也不该散落在 Provider 里：
//   · Provider 回答"每帧怎么算"；本类回答"数据放在哪、什么时候重建/释放"。
//
// 【生命周期】由 `DeferredPipeline` 在 Provider 注册之前 `Initialize`，并通过步骤 1 补上的
// Provider 生命周期遍历（`DeferredPipeline::OnResize/Shutdown` → `IGIProvider::OnResize/Shutdown`）
// 间接调用本类的 `OnResize/Shutdown`。这与既有 GI 源的做法一致：底层 pass 自己
// `device->CreateTexture/CreateBuffer` 并自持（`GI_RSM` / `GI_IBL` / `GI_DDGI`），
// 帧图只 `ImportTexture`、不拥有。
//
// 【当前状态】只有设备句柄与尺寸（步骤 3 的骨架）。后续步骤在这里追加真正的资源：
//   步骤 8  Mesh SDF（128³/mesh，R16F，按 mesh 缓存 + 上限）
//   步骤 10 Global SDF（512³ 单层 → 4×256³ clipmap）
//   步骤 14 Surface Cache 页表与页状态机
//   步骤 23 Screen Probe 的 SH 缓冲
// ============================================================

#include "RHI/RHI.h"

namespace he::render {

/// Lumen 持久资源宿主（不参与每帧 orchestration —— 那是 `LumenProvider` 的事）
class LumenScene {
public:
    bool Initialize(rhi::IRHIDevice* device, u32 width, u32 height);
    void Shutdown();
    /// 视口尺寸变化：只重建与屏幕尺寸相关的资源；世界空间资源（atlas / clipmap）不在此重建
    void OnResize(u32 width, u32 height);

    [[nodiscard]] bool IsReady() const { return m_Device != nullptr; }
    [[nodiscard]] u32  GetWidth()  const { return m_Width; }
    [[nodiscard]] u32  GetHeight() const { return m_Height; }

private:
    rhi::IRHIDevice* m_Device = nullptr;
    u32 m_Width  = 0;
    u32 m_Height = 0;
};

} // namespace he::render
