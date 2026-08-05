#pragma once
#include "RHI/RHI.h"
#include <memory>

namespace he::render {

// ============================================================
// STBNTexture — 时空蓝噪声 3D 纹理（128×128×64 RGBA8）
// 数据来自内嵌头文件 STBNData.h（一次性离线生成）。
// 供 PT/ReSTIR shader 通过 Load() 采样（无采样器，规避组合采样器坑）。
// ============================================================
class STBNTexture {
public:
    bool Initialize(rhi::IRHIDevice* device);
    void Shutdown();
    rhi::IRHITexture* GetTexture() const { return m_Texture.get(); }
    bool IsValid() const { return m_Texture != nullptr; }

private:
    rhi::IRHIDevice* m_Device = nullptr;
    std::unique_ptr<rhi::IRHITexture> m_Texture;
};

} // namespace he::render
