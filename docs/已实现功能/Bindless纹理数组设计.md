# Bindless 纹理数组设计

> 日期：2026-07-03 | 状态：设计完成 | Phase：P2

## 1. 目标

用全局 `Texture2D[]` + `SamplerState[]` 无界数组替换 per-mesh descriptor set（set=1），
所有 mesh 共享 set=0 完成绘制，消除单个 draw 的 descriptor set 切换，为 GPU Culling + Indirect Draw 铺路。

## 2. 架构

```
BindlessTextureManager（全局单例）
  ├── 纹理注册表：path → index
  ├── 全局 Texture2D[] 描述符（set=0, binding=5, bindless=true）
  ├── 全局 Sampler[] 描述符（set=0, binding=6, bindless=true）
  │
  └── glTF 加载时：RegisterTexture → 返回 index
                           │
                           ▼
                  GPUObjectData.materialID = index（4 张纹理的基索引）
```

每个 material 占 4 个连续纹理索引：BaseColor(0), Normal(1), MetallicRoughness(2), Occlusion(3)。

## 3. 文件清单

| 操作 | 文件 |
|------|------|
| 新增 | `Engine/Asset/Public/Asset/BindlessTextureManager.h` |
| 新增 | `Engine/Asset/Private/BindlessTextureManager.cpp` |
| 修改 | `Engine/Asset/Private/glTFLoader.cpp` — 加载纹理时注册 |
| 修改 | `Engine/Render/Pipeline/DeferredPipeline.h/.cpp` — 删除 set=1，全局 bindless |
| 修改 | `Engine/Render/Pipeline/ForwardPipeline.h/.cpp` — 同上 |
| 修改 | `Engine/Shader/Shaders/GBuffer.frag.slang` — Texture2D[] |
| 修改 | `Engine/Shader/Shaders/PBR.frag.slang` — 同上 |
| 修改 | `Engine/Shader/Shaders/PBR.vert.slang` — 同上 |
| 修改 | `Engine/Shader/Shaders/common.slang` — 共享 bindless 声明 |
| 修改 | `Engine/Scene/Public/Scene/MeshComponent.h` — 删除 m_DescSetHandle |

## 4. 接口

```cpp
class BindlessTextureManager {
public:
    static BindlessTextureManager& Instance();

    /// 注册纹理，返回 bindless 数组索引
    u32 RegisterTexture(rhi::IRHITexture* texture, rhi::IRHISampler* sampler);
    /// 注册完整 PBR material（4 张纹理），返回 materialID（基索引）
    u32 RegisterMaterial(rhi::IRHITexture* baseColor, rhi::IRHISampler* bcSamp,
                         rhi::IRHITexture* normal,   rhi::IRHISampler* nSamp,
                         rhi::IRHITexture* metallicRoughness, rhi::IRHISampler* mrSamp,
                         rhi::IRHITexture* occlusion, rhi::IRHISampler* ocSamp);

    /// 更新描述符集（纹理数组扩容或首次创建时调用）
    void UpdateDescriptorSet(rhi::IRHIDevice* device,
                             rhi::DescriptorSetHandle set,
                             u32 textureBinding,
                             u32 samplerBinding);

    u32 GetTextureCount() const;
    u32 GetSamplerCount() const;
};
```

## 5. Descriptor Layout

```
set=0（per-frame）:
  binding 2: StructuredBuffer<GPUObjectData> u_Objects  (现有)
  binding 5: Texture2D[]      u_Textures  bindless=true  (新增)
  binding 6: SamplerState[]   u_Samplers  bindless=true  (新增)
  // 删除 binding 5-8 in set=1（per-mesh）
```

## 6. Shader 使用

```hlsl
// common.slang
[[vk::binding(5, 0)]] Texture2D<float4> u_Textures[];
[[vk::binding(6, 0)]] SamplerState      u_Samplers[];

// GBuffer.frag
uint texBase = u_Objects[objectIndex].materialID;
float4 baseColor = u_Textures[texBase + 0].Sample(u_Samplers[0], uv);
float3 normal    = u_Textures[texBase + 1].Sample(u_Samplers[0], uv).rgb;
float2 mr        = u_Textures[texBase + 2].Sample(u_Samplers[0], uv).gb;
float  ao        = u_Textures[texBase + 3].Sample(u_Samplers[0], uv).r;
```

## 7. 测试

- 03.Sponza (Forward) + 04.Deferred 正常渲染
- RenderDoc 验证：Draw 之间无 descriptor set 切换
- 无 Vulkan 验证层告警
