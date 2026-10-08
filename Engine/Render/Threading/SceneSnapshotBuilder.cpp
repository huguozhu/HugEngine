#include "Threading/SceneSnapshotBuilder.h"

#include "Pipeline/PhysicalLight.h"   // KelvinToRGB / kPhysicalLightExposure（纯函数，无 RHI 依赖）
#include "Scene/LightComponent.h"
#include "Scene/SceneGraph.h"
#include "Scene/World.h"

#include <glm/gtx/norm.hpp>           // glm::normalize（与 CollectLights 用的是同一个）

namespace he::render {

u32 SceneSnapshotBuilder::BuildLights(he::World& world, he::SceneGraph& sg,
                                     const SceneSnapshotResolvers& resolvers,
                                     FrameSceneSnapshot& out,
                                     const SceneSnapshotLightOptions& options) {
    out.lights.clear();

    const bool physicalUnits = resolvers.physicalUnitsEnabled;

    // 与旧路径（`DeferredPipeline::CollectLights` / `ForwardPipeline::CollectLights`）逐字段对齐。
    // 注意：**未知 `LightType` 也要落一条**（旧路径的 `default: break` 之后照样写入并计数）——
    // 这里保持同样行为，避免"同场景收集数不同"这种最难查的差异。
    auto collect = [&](he::Entity e, he::LightComponent& lc) {
        if (!lc.enabled) return;
        if (out.lights.size() >= kGPUMaxLights) return;   // 与 MAX_LIGHTS(=kGPUMaxLights) 同口径

        SnapshotLight light{};

        // 色温 → RGB（叠加到颜色滤镜色上；colorTemperature <= 0 表示不启用色温）
        float3 lightColor = lc.color;
        if (lc.colorTemperature > 0.0f) {
            lightColor *= render::KelvinToRGB(lc.colorTemperature);
        }
        light.colorIntensity = float4(lightColor, lc.intensity);
        light.shadowIndex    = resolvers.shadowIndex ? resolvers.shadowIndex(e) : -1;
        // shadowRadius 只有 PathTracing 会写（它把软阴影半径交给路径追踪本身用）；
        // Deferred / Forward 保持 0（旧行为）
        light.shadowRadius   = options.writeShadowRadius ? lc.shadowRadius : 0.0f;

        switch (lc.type) {
        case he::LightType::Directional: {
            auto* dl = static_cast<he::DirectionalLight*>(&lc);
            light.directionType = float4(dl->direction, 0.0f);
            light.positionRange = float4(0.0f, 0.0f, 0.0f, 0.0f);
            // 物理模式：照度 lux → 渲染强度；`positionRange.w = -1` 是**物理模式标记**（着色器取 abs）
            if (IsPhysicalLightEnabled(physicalUnits, lc.illuminance)) {
                light.colorIntensity.w = lc.illuminance * kPhysicalLightExposure;
                light.positionRange.w  = -1.0f;
            }
            break;
        }
        case he::LightType::Point: {
            auto* pl = static_cast<he::PointLight*>(&lc);
            light.positionRange = float4(sg.GetWorldPosition(e), pl->range);
            // xyz 口径按选项（默认 0 = Deferred 现行行为）；w 才是着色器用来区分类型的字段
            light.directionType = options.pointLightWritesDirection
                                ? float4(0.0f, -1.0f, 0.0f, 1.0f)
                                : float4(0.0f, 0.0f, 0.0f, 1.0f);
            if (IsPhysicalLightEnabled(physicalUnits, lc.luminousIntensity)) {
                light.colorIntensity.w = lc.luminousIntensity * kPhysicalLightExposure;
                light.positionRange.w  = -(pl->range);               // 负范围 = 物理模式标记
            }
            break;
        }
        case he::LightType::Spot: {
            auto* sl = static_cast<he::SpotLight*>(&lc);
            const bool physical = IsPhysicalLightEnabled(physicalUnits, lc.luminousIntensity);
            light.positionRange = float4(sg.GetWorldPosition(e), physical ? -(sl->range) : sl->range);
            // 归一化口径按选项（默认归一化 = Deferred 口径；Forward 未归一化，属已登记漂移）
            light.directionType = options.normalizeSpotDirection
                                ? float4(glm::normalize(sl->direction), 2.0f)
                                : float4(sl->direction, 2.0f);
            light.coneAngles    = float2(sl->innerConeAngle, sl->outerConeAngle);
            if (physical) {
                light.colorIntensity.w = lc.luminousIntensity * kPhysicalLightExposure;
            }
            break;
        }
        case he::LightType::Rect: {
            // 只有 Forward 收集 Rect 光（见头文件登记的口径漂移①）：Deferred / PathTracing 走
            // `includeRectLights = false`，它们连遍历都不做 ⇒ 收集数也不同（这一点必须由调用方口径决定）
            auto* rl = static_cast<he::RectLight*>(&lc);
            light.positionRange = float4(sg.GetWorldPosition(e), rl->range);
            light.directionType = float4(rl->normal, 3.0f);      // w=3 标记 Rect；xyz=发光面法线
            light.coneAngles    = float2(rl->width, rl->height); // 复用：x=宽度, y=高度
            // 注意：Forward 的 Rect 分支**不**把范围取负（只有 colorIntensity.w 走物理换算）
            if (IsPhysicalLightEnabled(physicalUnits, lc.luminousIntensity)) {
                light.colorIntensity.w = lc.luminousIntensity * kPhysicalLightExposure;
            }
            break;
        }
        default:
            break;   // 未知类型：仍然落一条（与旧路径一致）
        }

        out.lights.push_back(light);
    };

    // 遍历顺序必须与旧路径一致：方向光 → 点光 → 聚光（→ Rect，仅 Forward 口径）
    // （光源下标会影响阴影索引与着色器里的光源循环次序）
    world.ForEach<he::DirectionalLight>(collect);
    world.ForEach<he::PointLight>(collect);
    world.ForEach<he::SpotLight>(collect);
    if (options.includeRectLights) {
        world.ForEach<he::RectLight>(collect);
    }

    return static_cast<u32>(out.lights.size());
}

} // namespace he::render
