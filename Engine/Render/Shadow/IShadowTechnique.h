#pragma once

#include "Pipeline/Material.h"
#include "RHI/RHI.h"
#include "Core/Types.h"
#include "Scene/Entity.h"
// 阶段 1 §15.1 第③段第 2 批：技术接口改吃快照（不再收 World/SceneGraph）
#include "Threading/FrameSceneSnapshot.h"
#include <memory>
#include <vector>

namespace he { class World; class SceneGraph; }
namespace he::scene { class LightComponent; }
namespace he::render { struct CameraData; class MeshRegistry; }

namespace he::render {

// ============================================================================
// IShadowTechnique — 单种阴影技术接口
//
// 每种技术负责一种光源类型的阴影（方向光 CSM / 点光源 Cubemap / RT）
// ShadowSystem 作为组合器持有多个 Technique，统一编排。
//
// 【阶段 1 §15.1 第③段第 2 批】收集与绘制都改为消费 `FrameSceneSnapshot`：
//   · `CollectLights` 从 `snapshot.shadowLights` 里过滤**自己那一类**光源（跨类型顺序无关，
//     同类型内保持实体顺序 ⇒ 推出的 `GPUShadowData` 序列与旧实现逐条一致）；
//   · `Render` 从 `snapshot.draws` 里取 `bShadowCaster` 的条目、按 `meshIndex` 查
//     `MeshRegistry` 拿顶点/索引缓冲 —— 遍历顺序与旧的 `Mesh→Cube→Sphere` 完全一致。
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
    virtual u32 CollectLights(const FrameSceneSnapshot& snapshot,
                               const CameraData& camera,
                               std::vector<GPUShadowData>& outData,
                               std::vector<he::Entity>& outEntities) = 0;

    // 渲染所有阴影 Pass（使用已收集的 ShadowData）
    virtual void Render(rhi::IRHICommandList* cmd,
                        const FrameSceneSnapshot& snapshot,
                        const MeshRegistry& registry,
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
