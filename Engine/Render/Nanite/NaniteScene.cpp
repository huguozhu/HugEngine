// ============================================================
// Nanite/NaniteScene.cpp — 数据宿主的生命周期桩（任务 1）
//
// 【本文件由 §14.8 任务 1 建立骨架，内容由任务 3/5/12 填充】
//   任务 1 只有"记住设备与尺寸 / 清空"这几个动作，**没有任何 GPU 资源**。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法）；不得依赖 `MeshBatcher` 的运行时状态
//   —— 它只当**一次性输入**。本文件里没有、也不允许有这些依赖。
// ============================================================

#include "Nanite/NaniteScene.h"

namespace he::render {

bool NaniteScene::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    // 任务 1：骨架就绪的判据就是"拿到设备"。后续任务在这里创建缓冲，
    // 并把"资源是否真的建成"纳入这个返回值。
    m_Device = device;
    m_Width  = width;
    m_Height = height;
    return m_Device != nullptr;
}

void NaniteScene::Shutdown() {
    // 任务 1 没有自持资源；后续任务在这里释放实例表/cluster 表/几何缓冲。
    m_Device = nullptr;
    m_Width  = 0;
    m_Height = 0;
}

void NaniteScene::OnResize(u32 width, u32 height) {
    m_Width  = width;
    m_Height = height;
}

} // namespace he::render
