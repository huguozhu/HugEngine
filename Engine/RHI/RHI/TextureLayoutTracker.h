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

// ============================================================
// 视图 → 底层图像 登记
//
// 为什么需要：Vulkan 的 image barrier 只能作用于 VkImage，而 render pass 的附件是以
// **视图**（VkImageView）形式传入的（`BeginOffscreenPass(void* colorView, void* depthView, …)`）。
// 因此在"开始 render pass 前按需补一次布局转换"时，需要由视图反查图像与 mip/layer 范围。
// ============================================================

/// 登记视图对应的底层图像与 mip / array layer 数量（纹理创建时调用）
/// @param format 纹理的 RHI 格式（u32(Format)）：render pass 边界回写真实布局时要用它
///               判断该附件结束在 PRESENT_SRC 还是 COLOR_ATTACHMENT（见 VulkanConverters.h）
void TrackViewImage(void* imageView, void* image, u32 mipLevels, u32 arrayLayers,
                    u32 format = 0);

/// 查询视图对应的图像与范围；返回 false 表示未登记
bool QueryViewImage(void* imageView, void*& outImage, u32& outMipLevels, u32& outArrayLayers);

/// 查询视图对应纹理的格式（u32(Format)）；返回 false 表示未登记
bool QueryViewFormat(void* imageView, u32& outFormat);

/// 纹理销毁时清理登记
void ForgetViewImage(void* imageView);

// ============================================================
// 「图像是否被写入过」追踪
//
// 动机（§9.2-S / §11.3.1）：RHI 允许纹理以未初始化状态被采样。这本是一个「消费者门控
// 写漏」的局部错误，却会被放大成**静默的物理错误**——读到 NaN/垃圾时画面只是看起来偏暗，
// 不报任何错——以及**读数跨构建不可复现**（未初始化显存的内容随内存布局变化）。
// 引擎里真实的例子：IBL 辐照度图在「IBL 不在漫反射层栈」时从不烘焙，而 DDGI 探针一直在
// 采它，于是 DDGI 静默退化成着色器里一条硬编码兜底常数，完全不做 GI。
//
// 这里只做**检测与告警**、不改写纹理内容：把「静默错误」变成一条带尺寸/格式的明确日志。
// 按**图像**而非视图记录——立方体贴图由逐面视图写入、却被整张 cube 视图采样。
//
// 判定带**累计阈值**：不是「第一次采样时没写过」就报警。同一帧内先采样后写入（读到上一帧
// 产物、pass 注册顺序靠后）是正常的，首帧尤其密集；这类情况只会出现一两次，随后图像被登记
// 为已写入、计数清零。真正的写漏每一帧都被采样到，很快越过阈值（约 30 帧）。
// ============================================================

/// 标记该视图所属的图像已被写入（渲染目标 / 清除 / 上传 / blit 目标）
void MarkViewWritten(void* imageView);

/// 查询该视图所属的图像是否被写入过。
/// 返回 false 表示**该图像已被判定为"采样时从未写入"且越过累计阈值**——此时采样它读到的
/// 是不确定值。outFirstWarn 为 true 表示这是该图像第一次给出该判定（供调用方只告警一次）。
/// 返回 true 涵盖三种情况：已写入、可疑次数未达阈值、已经告过警；视图未登记时也返回 true。
bool IsViewWritten(void* imageView, bool* outFirstWarn = nullptr);

} // namespace he::rhi
