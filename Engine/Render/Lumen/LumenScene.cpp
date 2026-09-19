#include "Lumen/LumenScene.h"

#include "Core/Log.h"

namespace he::render {

bool LumenScene::Initialize(rhi::IRHIDevice* device, u32 width, u32 height) {
    if (!device) {
        HE_CORE_ERROR("LumenScene: 设备为空，初始化失败");
        return false;
    }
    m_Device = device;
    m_Width  = width;
    m_Height = height;
    HE_CORE_INFO("LumenScene: 初始化持久资源宿主（视口 {}x{}）", width, height);
    return true;
}

void LumenScene::Shutdown() {
    // 步骤 8/10/14/23 在这里释放 Mesh SDF 缓存、Global SDF clipmap、页表与探针缓冲。
    // 当前只有句柄与尺寸，故仅清空状态。
    if (m_Device) {
        HE_CORE_INFO("LumenScene: 释放持久资源宿主");
    }
    m_Device = nullptr;
    m_Width  = 0;
    m_Height = 0;
}

void LumenScene::OnResize(u32 width, u32 height) {
    if (!m_Device) return;
    if (width == m_Width && height == m_Height) return;
    m_Width  = width;
    m_Height = height;
    // 屏幕尺寸相关的资源（如 Screen Probe 的 SH 缓冲）在后续步骤按新尺寸重建；
    // Surface Cache atlas 与 Global SDF clipmap 是**世界空间**尺寸，与视口无关，不在此重建。
    HE_CORE_INFO("LumenScene: 视口变化，屏幕尺寸相关资源待重建（{}x{}）", width, height);
}

} // namespace he::render
