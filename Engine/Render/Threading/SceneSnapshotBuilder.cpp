#include "Threading/SceneSnapshotBuilder.h"

#include "Pipeline/PhysicalLight.h"   // KelvinToRGB / kPhysicalLightExposure（纯函数，无 RHI 依赖）
#include "Scene/BillboardComponent.h"
#include "Scene/CubeComponent.h"
#include "Scene/DecalComponent.h"
#include "Scene/InstancedMeshComponent.h"
#include "Scene/LightComponent.h"
#include "Scene/MeshComponent.h"
#include "Scene/ParticleComponent.h"      // 粒子发射器进快照（T1.4）
#include "Scene/PhysicalSkyComponent.h"   // GetPhysicalSkySun（T1.4：环境参数进快照）
#include "Scene/SceneGraph.h"
#include "Scene/SkeletalMeshComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/TextRenderComponent.h"
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

u32 SceneSnapshotBuilder::BuildObjects(he::World& world, he::SceneGraph& sg, const CameraData& camera,
                                       const SceneSnapshotObjectOptions& options,
                                       const FrameSceneSnapshot* prev, FrameSceneSnapshot& out) {
    out.draws.clear();

    // 与 `GPUScene::Collect` 的**首次全量**分支逐条对齐：组件类型顺序、无索引跳过、
    // 贴花是否排除、广告牌/文字的相机对齐矩阵、世界 AABB、objectID = 收集序号。
    // 【为什么按精确类型遍历】组件存储是 `unordered_map<type_index, bucket>`（见 `Scene/World.h`），
    // `ForEach<T>` 只匹配**精确类型**；因此这里的类型清单必须与旧路径完全一致，否则收集集合会变。
    const auto addPlain = [&](he::Entity e, auto& comp) {
        const u32 id = static_cast<u32>(out.draws.size());
        CollectObjectItem(e, comp, sg.GetWorldMatrix(e), id, prev, out);
    };

    // 广告牌 / 3D 文字：矩阵对齐相机（每帧随相机变化；这也是"收集必须发生在有相机的帧里"的原因）
    const auto addBillboard = [&](he::Entity e, auto& comp) {
        const float4x4 base = sg.GetWorldMatrix(e);
        const float4x4 wm = BillboardComponent::MakeBillboardMatrix(
            float3(base[3]), camera.forward, camera.up, comp.size);
        const u32 id = static_cast<u32>(out.draws.size());
        CollectObjectItem(e, comp, wm, id, prev, out);
    };

    world.ForEach<MeshComponent>([&](he::Entity e, MeshComponent& mc) { addPlain(e, mc); });
    world.ForEach<CubeComponent>([&](he::Entity e, CubeComponent& cc) { addPlain(e, cc); });
    world.ForEach<SphereComponent>([&](he::Entity e, SphereComponent& sc) { addPlain(e, sc); });
    world.ForEach<BillboardComponent>([&](he::Entity e, BillboardComponent& bb) { addBillboard(e, bb); });
    world.ForEach<TextRenderComponent>([&](he::Entity e, TextRenderComponent& tr) { addBillboard(e, tr); });
    if (!options.excludeDecals) {
        world.ForEach<DecalComponent>([&](he::Entity e, DecalComponent& dc) { addPlain(e, dc); });
    }
    world.ForEach<InstancedMeshComponent>([&](he::Entity e, InstancedMeshComponent& im) { addPlain(e, im); });
    // 骨骼网格：除对象条目外，还要把**蒙皮矩阵**追加进快照的扁平数组（T1.4）
    world.ForEach<SkeletalMeshComponent>([&](he::Entity e, SkeletalMeshComponent& sm) {
        const u32 before = static_cast<u32>(out.draws.size());
        addPlain(e, sm);
        if (out.draws.size() > before) {
            AppendSkinMatrices(out.draws.back(), sm, out);
        }
    });

    return static_cast<u32>(out.draws.size());
}

void SceneSnapshotBuilder::AppendSkinMatrices(SnapshotDrawItem& item, const he::SkeletalMeshComponent& comp,
                                             FrameSceneSnapshot& out) {
    item.skinMatrixOffset = static_cast<u32>(out.skinMatrices.size());
    item.skinMatrixCount  = static_cast<u32>(comp.boneMatrices.size());
    out.skinMatrices.insert(out.skinMatrices.end(), comp.boneMatrices.begin(), comp.boneMatrices.end());
}

bool SceneSnapshotBuilder::BuildEnvironment(he::World& world, FrameSceneSnapshot& out) {    // 与两处旧调用点逐字段一致：找不到/未启用物理天空时，方向保持 (0,1,0)、浑浊度归 0
    // （Forward 的注释写明"天空移除时复位浑浊度=0"）。
    float3 sunDir    = float3(0.0f, 1.0f, 0.0f);
    float  turbidity = 0.0f;
    const bool found = he::GetPhysicalSkySun(world, sunDir, turbidity);
    out.atmosphere = float4(sunDir, turbidity);
    return found;
}

u32 SceneSnapshotBuilder::BuildParticles(he::World& world, FrameSceneSnapshot& out) {
    out.particles.clear();
    world.ForEach<he::ParticleComponent>([&](he::Entity, he::ParticleComponent& pc) {
        SnapshotParticleEmitter emitter;
        emitter.rendererId   = pc.rendererId;             // 由注册方回填（见组件上的字段注释）
        emitter.params       = pc.GetParam();             // 发射/模拟参数整份按值带走（帧内不再读组件）
        emitter.emitPosition = pc.GetWorldEmitPosition();
        out.particles.push_back(emitter);
    });
    return static_cast<u32>(out.particles.size());
}

} // namespace he::render
