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

    /// 每帧注入"当前飞行帧槽位"（取值 `[0, MAX_FRAMES_IN_FLIGHT)`）。
    /// 【为什么必须有】持有**每帧被清空重画**的 GPU 资源的技术，必须把该资源按飞行帧分份：
    /// 只有一份时，上一帧仍在执行的命令会采到"正在清空/重画"的中间态（相机移动时表现为阴影
    /// 闪动/突然消失）。CSM / Point / Spot / Rect 四类贴图都属此类，故都覆写本函数。
    virtual void SetFrameSlot(u32 /*slot*/) {}

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
    /// **当前飞行帧槽位**的第 index 张阴影贴图（CSM: index=级联号；Point/Spot/Rect: index=0）
    virtual rhi::IRHITexture* GetShadowMap(u32 index) const = 0;
    /// **指定飞行帧槽位**的第 index 张 —— 供管线初始化时把每个槽位的贴图分别绑进对应描述符集。
    /// 【为什么需要它】阴影贴图都是"每帧清空重画、同帧采样"，必须按飞行帧分份（见 SetFrameSlot）；
    /// 而初始化时 `m_FrameSlot` 还是 0，只能显式按槽位取。
    virtual rhi::IRHITexture* GetShadowMapAt(u32 slot, u32 index) const = 0;
    virtual rhi::IRHISampler* GetShadowSampler() const = 0;

    // 点光源阴影纹理（仅 PointShadowTechnique 覆写）。`slot` = 飞行帧槽位：点光立方体阴影图
    // 是**逐帧重画**的资源，必须按槽位取用（见 `SetFrameSlot` 的说明）。
    virtual rhi::IRHITexture* GetPointShadowMap(u32 /*slot*/) const { return nullptr; }
    virtual rhi::IRHISampler* GetPointShadowSampler() const { return nullptr; }
};

} // namespace he::render
