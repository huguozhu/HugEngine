#include "RT/STBNTexture.h"
#include "Core/Log.h"
#include "STBNData.h"   // kSTBNData / kSTBNWidth / kSTBNHeight / kSTBNFrames

namespace he::render {

bool STBNTexture::Initialize(rhi::IRHIDevice* device) {
    m_Device = device;
    if (m_Texture.IsValid()) return true;   // 幂等

    // 3D 纹理：initialData 由 Vulkan 后端走暂存缓冲上传（见 VulkanResources.cpp）
    rhi::TextureDesc d;
    d.width  = kSTBNWidth;
    d.height = kSTBNHeight;
    d.depth  = kSTBNFrames;                    // 64 层（时间维）
    d.mipLevels = 1;
    d.format = rhi::Format::RGBA8_UNORM;
    d.usage  = rhi::TextureUsage::ShaderResource;
    d.initialData = kSTBNData;                 // 内嵌数据（4MB）
    m_Texture = device->CreateTextureHandle(d);   // A-3
    if (!m_Texture.IsValid()) {
        HE_CORE_ERROR("STBNTexture: 3D 纹理创建失败");
        return false;
    }
    HE_CORE_INFO("STBNTexture: 初始化完成 ({}x{}x{}, 内嵌数据 {}B)",
                 kSTBNWidth, kSTBNHeight, kSTBNFrames,
                 kSTBNWidth * kSTBNHeight * kSTBNFrames * 4);
    return true;
}

void STBNTexture::Shutdown() {
    // 【空保护】m_Device 可能为空（Shutdown 被调用两次，或 Initialize 失败后进入清理路径），
    // 直接解引用会崩溃（实测：02.Cube 退出时连续崩在多个 Shutdown 里）。
    if (!m_Device) return;

    m_Device->DestroyTextureHandle(m_Texture); m_Texture = {};
    m_Device = nullptr;
}

} // namespace he::render
