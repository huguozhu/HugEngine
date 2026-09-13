#pragma once

// ============================================================
// RHI/Vulkan/VulkanTextureLiveness.h — 纹理存活登记（诊断用）
//
// 背景（一次真实崩溃的根因调查）：
//   06.GILab 出现约 1/40 的偶发崩溃，崩溃点定位在
//   `he::rhi::VulkanTexture::GetImageView()` 函数体内部（符号 +0xB），
//   即"通过一个无效的 IRHITexture* 调用成员 getter"——指针不是 null
//   （各调用点都有 null 检查），而是**已销毁纹理留下的野指针**。
//   野指针有时仍指向可读内存（于是静默读到脏 VkImageView），有时该内存
//   已被系统解映射，此时读取 m_ImageView 就触发访问违例 → 偶发崩溃。
//
// 本登记表的作用：
//   让"纹理对象的生死"可被查询，从而在**解引用之前**判定指针是否仍然有效。
//   这样野指针会变成一条明确的错误日志（含绑定号与指针值）+ 安全跳过，
//   而不是一个无法定位的访问违例。
//
// 开销：纹理创建/销毁各一次哈希写（带互斥锁），仅作诊断，非热路径。
// ============================================================

namespace he::rhi {

/// 登记一个纹理对象（在 VulkanTexture 构造完成时调用）
void MarkTextureAlive(const void* texture);

/// 注销一个纹理对象（在 VulkanTexture 析构时调用）
void MarkTextureDead(const void* texture);

/// 查询该指针当前是否是"存活中的纹理对象"
/// 注意：对任意（包括野）指针值都是安全的——只做哈希查找，不解引用
bool IsTextureAlive(const void* texture);

/// 当前存活纹理数量（日志/面板用）
unsigned int GetAliveTextureCount();

} // namespace he::rhi
