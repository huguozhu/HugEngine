// ============================================================
// Nanite/NaniteUpload.cpp — 上传段的生命周期桩（任务 1）
//
// 【本文件由 §14.8 任务 1 建立骨架，内容由任务 12 填充】
//   任务 1 只有"记住设备与尺寸 / 清空"这几个动作，**没有任何 GPU 资源**。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法）；不得依赖 `MeshBatcher` 的运行时状态
//   —— 它只当**一次性输入**。故本文件（以及未来的实现）不得出现 `MeshBatcher*` 成员。
// ============================================================

#include "Nanite/NaniteUpload.h"

namespace he::render {

bool NaniteUpload::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    // 任务 1：骨架就绪 = 拿到设备。任务 12 起在这里建暂存缓冲与目标缓冲，
    // 并把"缓冲是否真的建成"纳入这个返回值。
    m_Device = device;
    m_Width  = width;
    m_Height = height;
    return m_Device != nullptr;
}

void NaniteUpload::Shutdown() {
    // 任务 1 没有自持资源；任务 12 起在这里释放暂存/目标缓冲。
    m_Device = nullptr;
    m_Width  = 0;
    m_Height = 0;
}

void NaniteUpload::OnResize(u32 width, u32 height) {
    // 上传段的资源与世界空间挂钩，不随视口变化；这里只记录尺寸以便后续诊断。
    m_Width  = width;
    m_Height = height;
}

} // namespace he::render
