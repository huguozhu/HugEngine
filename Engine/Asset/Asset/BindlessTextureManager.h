#pragma once

#include "RHI/RHI.h"
#include "RHI/Buffer.h"
#include <vector>

// ============================================================
// BindlessTextureManager — 全局 bindless 纹理/采样器数组管理器
//
// 管理全局 Texture2D[] + SamplerState[] 描述符数组（通常 set=0 binding 5-6），
// 所有 mesh 共享同一描述符集，无需 per-mesh descriptor set 切换。
// 每个 material 占 4 个连续纹理索引：BaseColor(0), Normal(1), MetallicRoughness(2), Occlusion(3)。
//
// 设计原则：管线初始化时 RegisterDescriptorSet() 注册全部需同步的描述符集，
// 纹理加载后 FlushPending() 自动向全部已注册 set 推送，无需调用方手动遍历。
// 这样三缓冲等多描述符集场景天然正确，不会遗漏更新。
// ============================================================

namespace he::asset {

class BindlessTextureManager {
public:
    static BindlessTextureManager& Instance();

    // --- 描述符集注册（管线初始化时调用，每个需同步的 set 调用一次） ---

    /// 注册一个需要同步纹理数组的描述符集
    /// 后续 FlushPending() 会自动向全部已注册 set 写入纹理/采样器数组
    void RegisterDescriptorSet(rhi::IRHIDevice* device,
                               rhi::DescriptorSetHandle set,
                               u32 textureBinding,
                               u32 samplerBinding);

    // --- 纹理/材质注册 ---

    /// 注册单张纹理，返回 texture 数组索引（自动标记待推送）
    u32 RegisterTexture(rhi::IRHITexture* texture, rhi::IRHISampler* sampler);

    /// 注册完整 PBR material（4 张纹理 + 采样器），返回 materialID（基索引）
    u32 RegisterMaterial(rhi::IRHITexture* baseColor, rhi::IRHISampler* bcSamp,
                         rhi::IRHITexture* normal,   rhi::IRHISampler* nSamp,
                         rhi::IRHITexture* metallicRoughness, rhi::IRHISampler* mrSamp,
                         rhi::IRHITexture* occlusion, rhi::IRHISampler* ocSamp);

    // --- 同步 ---

    /// 如有待推送的纹理变更，写入全部已注册描述符集（无变更时无操作）
    /// 应在每帧渲染场景前调用
    void FlushPending();

    // --- 查询 ---

    u32 GetTextureCount() const { return m_TextureCount; }
    bool HasPendingFlush() const { return m_PendingFlush; }

    /// 设置占位纹理/采样器（null 纹理回退用，必须在注册纹理前调用）
    void SetDefaultTexture(rhi::IRHITexture* tex, rhi::IRHISampler* samp) {
        m_DefaultTexture = tex; m_DefaultSampler = samp;
    }

private:
    BindlessTextureManager() = default;

    // 已注册的描述符集（需同步纹理数组的 set）
    struct RegisteredSet {
        rhi::IRHIDevice*          device;
        rhi::DescriptorSetHandle  set;
        u32                       textureBinding;
        u32                       samplerBinding;
    };
    std::vector<RegisteredSet> m_RegisteredSets;

    // 纹理/采样器数组
    std::vector<rhi::IRHITexture*> m_Textures;
    std::vector<rhi::IRHISampler*> m_Samplers;
    rhi::IRHITexture* m_DefaultTexture = nullptr;
    rhi::IRHISampler* m_DefaultSampler = nullptr;
    u32 m_TextureCount = 0;

    // 是否有未推送的纹理变更
    bool m_PendingFlush = false;
};

} // namespace he::asset
