#pragma once

#include "RHI/RHI.h"
#include "RHI/Buffer.h"
#include <vector>

// ============================================================
// BindlessTextureManager — 全局 bindless 纹理/采样器数组管理器
//
// 管理一个全局 Texture2D[] + SamplerState[] 描述符数组（set=0 binding 5-6），
// 所有 mesh 共享同一描述符集，无需 per-mesh descriptor set 切换。
// 每个 material 占 4 个连续纹理索引：BaseColor(0), Normal(1), MetallicRoughness(2), Occlusion(3)。
// ============================================================

namespace he::asset {

class BindlessTextureManager {
public:
    static BindlessTextureManager& Instance();

    /// 注册单张纹理，返回 texture 数组索引
    u32 RegisterTexture(rhi::IRHITexture* texture, rhi::IRHISampler* sampler);

    /// 注册完整 PBR material（4 张纹理 + 采样器），返回 materialID（基索引）
    u32 RegisterMaterial(rhi::IRHITexture* baseColor, rhi::IRHISampler* bcSamp,
                         rhi::IRHITexture* normal,   rhi::IRHISampler* nSamp,
                         rhi::IRHITexture* metallicRoughness, rhi::IRHISampler* mrSamp,
                         rhi::IRHITexture* occlusion, rhi::IRHISampler* ocSamp);

    /// 更新描述符集（纹理数组扩容或首次创建时调用）
    void UpdateDescriptorSet(rhi::IRHIDevice* device,
                             rhi::DescriptorSetHandle set,
                             u32 textureBinding,
                             u32 samplerBinding);

    u32 GetTextureCount() const { return m_TextureCount; }

    /// 设置占位纹理/采样器（null 纹理回退用，必须在注册纹理前调用）
    void SetDefaultTexture(rhi::IRHITexture* tex, rhi::IRHISampler* samp) {
        m_DefaultTexture = tex; m_DefaultSampler = samp;
    }

private:
    BindlessTextureManager() = default;
    std::vector<rhi::IRHITexture*> m_Textures;
    std::vector<rhi::IRHISampler*> m_Samplers;
    rhi::IRHITexture* m_DefaultTexture = nullptr;
    rhi::IRHISampler* m_DefaultSampler = nullptr;
    u32 m_TextureCount = 0;
    bool m_DescDirty = true;
};

} // namespace he::asset
