// ============================================================
// Nanite/NaniteCull.cpp — 剔除段的生命周期桩（任务 1）
//
// 【本文件由 §14.8 任务 1 建立骨架，内容由任务 3 起填充】
//   任务 1 只有"记住设备与尺寸 / 清空"这几个动作，**不派发 compute、不建缓冲**。
//
// 【§14.3 依赖禁令】模块内不得引用 `GI_*` / `Lumen*` / `GPUCulling` 的内部结构
//   （可借其 Hi-Z 纹理句柄与描述符写法）；不得依赖 `MeshBatcher` 的运行时状态
//   —— 它只当**一次性输入**。本文件不得 include `Pipeline/GPUCulling.h`。
// ============================================================

#include "Nanite/NaniteCull.h"

namespace he::render {

bool NaniteCull::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    // 任务 1：骨架就绪 = 拿到设备。任务 3 起在这里建计数/间接命令缓冲与 compute PSO，
    // 并把"资源是否真的建成"纳入这个返回值。
    m_Device = device;
    m_Width  = width;
    m_Height = height;
    return m_Device != nullptr;
}

void NaniteCull::Shutdown() {
    // 任务 1 没有自持资源；任务 3 起在这里释放计数/间接命令缓冲与 PSO。
    m_Device = nullptr;
    m_Width  = 0;
    m_Height = 0;
}

void NaniteCull::OnResize(u32 width, u32 height) {
    // Hi-Z 金字塔随视口变化，故这里必须记录尺寸（任务 3 起据此重建金字塔相关资源）。
    m_Width  = width;
    m_Height = height;
}

} // namespace he::render
