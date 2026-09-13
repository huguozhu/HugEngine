#pragma once

// ============================================================
// RHI/TextureLayoutTracker.h — 纹理当前布局追踪（跨帧真实布局）
//
// 用途（一次实测问题的正解）：
//   RenderGraph 每帧重建，对**导入纹理**（GBuffer depth、Lighting HDR depth 等
//   跨帧持久的资源）在每帧开始时把状态假设为 `Undefined`，于是"首次使用不发射
//   barrier"。但这些图在**上一帧结束时实际停在某个布局**（例如深度图停在
//   DEPTH_STENCIL_READ_ONLY），导致：
//     · vkCmdBeginRenderPass 报 initialLayout 不符（VUID-...-initialLayout-00900，1 次/帧）
//     · 紧随其后的 barrier 声明错误的 oldLayout（VUID-...-oldLayout-01197，1 次/帧）
//   本模块让 RHI 记录每个纹理视图的**真实布局**，供图在首次使用时查询。
//
// 键：纹理视图句柄（`IRHITexture::GetNativeHandle()` 返回的 VkImageView）。
//     barrier 路径与 render pass 路径都能直接拿到该句柄，无需额外反查。
//
// 已知局限：当前只由 **barrier** 路径更新；render pass 自身的 initial/final 布局
//   转变尚未记录（引擎的 render pass 会把深度留在 READ_ONLY，与 barrier 路径的记录
//   在多数情况下一致，故本次足以解决问题；后续如需更严格可补 render pass 挂钩）。
// ============================================================

#include "RHI/Types.h"

namespace he::rhi {

/// 记录纹理视图当前的资源状态（布局）
void TrackTextureLayout(void* imageView, ResourceState state);

/// 查询纹理视图当前记录的状态；返回 false 表示从未记录过（该图还没被任何 barrier 使用）
bool QueryTrackedTextureLayout(void* imageView, ResourceState& outState);

/// 纹理销毁时清理记录（避免句柄复用造成误判）
void ForgetTrackedTextureLayout(void* imageView);

} // namespace he::rhi
