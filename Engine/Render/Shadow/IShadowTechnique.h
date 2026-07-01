#pragma once

#include "Pipeline/Material.h"
#include "RHI/RHI.h"
#include "Core/Types.h"
#include "Scene/Entity.h"
#include <memory>
#include <vector>

namespace he { class World; class SceneGraph; }
namespace he::scene { class LightComponent; }
namespace he::render { struct CameraData; }

namespace he::render {

// ============================================================================
// IShadowTechnique — 单种阴影技术接口
//
// 每种技术负责一种光源类型的阴影（方向光 CSM / 点光源 Cubemap / RT）
// ShadowSystem 作为组合器持有多个 Technique，统一编排。
// ============================================================================
class IShadowTechnique {
public:
    virtual ~IShadowTechnique() = default;

    virtual const char* GetName() const = 0;

    // 生命周期
    virtual bool Initialize(rhi::IRHIDevice* device) = 0;
    virtual void Shutdown() = 0;

    // 每帧注入共享资源
    virtual void SetRenderResources(rhi::IRHIBuffer* objBuf,
                                     rhi::DescriptorSetHandle descSet) = 0;

    // 收集该类型的光源，填充 GPUShadowData 和 Entity 列表
    // 返回新增条目数
    virtual u32 CollectLights(he::World& world, he::SceneGraph& sg,
                               const CameraData& camera,
                               std::vector<GPUShadowData>& outData,
                               std::vector<he::Entity>& outEntities) = 0;

    // 渲染所有阴影 Pass（使用已收集的 ShadowData）
    virtual void Render(rhi::IRHICommandList* cmd,
                        he::World& world, he::SceneGraph& sg,
                        const std::vector<GPUShadowData>& shadowData,
                        u32 dataStartIndex) = 0;

    // Shadow Map 访问器
    virtual u32 GetShadowMapCount() const = 0;
    virtual rhi::IRHITexture* GetShadowMap(u32 index) const = 0;
    virtual rhi::IRHISampler* GetShadowSampler() const = 0;

    // 点光源阴影纹理（仅 PointShadowTechnique 覆写）
    virtual rhi::IRHITexture* GetPointShadowMap() const { return nullptr; }
    virtual rhi::IRHISampler* GetPointShadowSampler() const { return nullptr; }
};

} // namespace he::render
