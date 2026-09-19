// ============================================================
// Nanite/NaniteRaster.cpp — 光栅段的生命周期桩（任务 1）
//
// 【本文件由 §14.8 任务 1 建立骨架，内容由任务 4 起填充】
//   任务 1 只有"记住设备与尺寸 / 清空"这几个动作，**不建 PSO、不写 GBuffer**。
//   任务 1 的验收要求"开启档画面不变"，所以本文件在任务 1 里必须保持**零 GPU 副作用**。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法）；不得依赖 `MeshBatcher` 的运行时状态
//   —— 它只当**一次性输入**。
// ============================================================

#include "Nanite/NaniteRaster.h"

namespace he::render {

bool NaniteRaster::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    // 任务 1：骨架就绪 = 拿到设备。任务 4 起在这里建软光栅 PSO/描述符集，
    // 并把"PSO/附件布局是否真的可用"纳入这个返回值。
    m_Device = device;
    m_Width  = width;
    m_Height = height;
    return m_Device != nullptr;
}

void NaniteRaster::Shutdown() {
    // 任务 1 没有自持资源；任务 4 起在这里释放软光栅 PSO 与描述符集。
    m_Device = nullptr;
    m_Width  = 0;
    m_Height = 0;
}

void NaniteRaster::OnResize(u32 width, u32 height) {
    // 软光栅的目标是屏幕尺寸的 GBuffer ⇒ 这里必须记录尺寸（任务 4 起据此重建目标相关资源）。
    m_Width  = width;
    m_Height = height;
}

} // namespace he::render
