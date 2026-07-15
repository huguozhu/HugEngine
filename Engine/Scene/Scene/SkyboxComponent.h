#pragma once

#include "Scene/Component.h"
#include "RHI/RHI.h"
#include "Math/Math.h"
#include <memory>

// ============================================================
// SkyboxComponent — 天空盒组件
//
// 持有 Cubemap 纹理 + 采样器，由 ForwardPipeline 渲染。
// 用法: world.AddComponent<SkyboxComponent>(entity);
//       component->SetCubemap(tex, sampler);
// ============================================================

namespace he {

class SkyboxComponent : public Component {
    HE_COMPONENT()
public:
    void OnCreate() override;

    // 设置天空盒 Cubemap（外部加载的程序化纹理或文件纹理）
    void SetCubemap(std::unique_ptr<rhi::IRHITexture> tex,
                    std::unique_ptr<rhi::IRHISampler> sampler) {
        m_Cubemap        = std::move(tex);
        m_CubemapSampler = std::move(sampler);
    }

    rhi::IRHITexture* GetCubemap()        const { return m_Cubemap.get(); }
    rhi::IRHISampler* GetCubemapSampler() const { return m_CubemapSampler.get(); }

    // 天空盒参数
    float intensity = 1.0f;   // 亮度
    float rotation  = 0.0f;   // 绕 Y 轴旋转（弧度）
    bool  enabled   = true;   // 是否渲染

private:
    std::unique_ptr<rhi::IRHITexture> m_Cubemap;
    std::unique_ptr<rhi::IRHISampler> m_CubemapSampler;
};

} // namespace he
