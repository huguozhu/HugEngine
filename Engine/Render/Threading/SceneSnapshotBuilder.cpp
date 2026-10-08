#include "Threading/SceneSnapshotBuilder.h"
// 共享注册器需要 MeshRegistryEntry 的完整定义
#include "Threading/MeshRegistry.h"

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
#include "Scene/SkyboxComponent.h"       // T1.4：天空盒进快照（渲染期不再读组件）
#include "Scene/SplineMeshComponent.h"   // 附录 E / E-4：与 SceneRenderer 的收集口径统一
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
    // 样条网格（附录 E / E-4 口径修正）：`SceneRenderer::Prepare` 一直会收集它，而这里原先漏了 ——
    // 两处枚举顺序不一致会让 `objectIndex` 错位（`FillGPUScene` 按顺序对齐），且 GPU 剔除/间接绘制
    // 会漏掉样条网格。**插入位置必须与 `SceneRenderer::Prepare` 一致**：Decal 之后、实例化之前。
    // 说明：当前样例中没有样条网格，无法做前后对比验证，本修正依据是"两处口径必须一致"这一硬约束。
    world.ForEach<SplineMeshComponent>([&](he::Entity e, SplineMeshComponent& spl) { addPlain(e, spl); });
    // 实例化网格：只登记对象条目（材质/世界变换），顶点由实例化 Pass 提供 ⇒ bInstanced=true
    world.ForEach<InstancedMeshComponent>([&](he::Entity e, InstancedMeshComponent& im) {
        const u32 id = static_cast<u32>(out.draws.size());
        CollectObjectItem(e, im, sg.GetWorldMatrix(e), id, prev, out, /*bInstanced=*/true);
    });
    // 骨骼网格：除对象条目外，还要把**蒙皮矩阵**追加进快照的扁平数组（T1.4）；
    // 顶点由蒙皮 Pass 提供 ⇒ bInstanced=true
    world.ForEach<SkeletalMeshComponent>([&](he::Entity e, SkeletalMeshComponent& sm) {
        const u32 before = static_cast<u32>(out.draws.size());
        const u32 id     = before;
        CollectObjectItem(e, sm, sg.GetWorldMatrix(e), id, prev, out, /*bInstanced=*/true);
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

PBRMaterial SceneSnapshotBuilder::MakePBRMaterial(const he::MeshComponent& comp) {
    // 【唯一实现】这段映射原先内联在 `SceneRenderer::Prepare` 里（E-3 之前）；抽到这里是为了让
    // "收集侧算材质"与"渲染侧算材质"共用一份口径 —— 否则两边会各自漂移（本方案已登记过 5 处同类漂移）。
    // 逐字段与 `SceneRenderer.cpp:110-125` 保持一致（含"不设置 disney 等扩展参数"这一点）。
    PBRMaterial mat = GetDefaultMaterial();
    mat.baseColorFactor = comp.baseColorFactor;
    mat.emissiveFactor  = comp.emissiveFactor;
    mat.metallicFactor  = comp.metallicFactor;
    mat.roughnessFactor = comp.roughnessFactor;
    mat.aoFactor        = comp.aoFactor;
    mat.alphaCutoff     = comp.alphaCutoff;
    mat.alphaMode       = static_cast<AlphaMode>(comp.alphaMode);
    mat.doubleSided     = comp.doubleSided;
    mat.unlit           = comp.unlit;
    // 纹理路径 → textureMask（无纹理槽 shader 不采样，避免占位纹理污染）
    mat.baseColorTexture         = comp.baseColorTexture;
    mat.normalTexture            = comp.normalTexture;
    mat.metallicRoughnessTexture = comp.metallicRoughnessTexture;
    mat.occlusionTexture         = comp.occlusionTexture;
    mat.emissiveTexture          = comp.emissiveTexture;
    return mat;
}

bool SceneSnapshotBuilder::BuildEnvironment(he::World& world, FrameSceneSnapshot& out) {    // 与两处旧调用点逐字段一致：找不到/未启用物理天空时，方向保持 (0,1,0)、浑浊度归 0
    // （Forward 的注释写明"天空移除时复位浑浊度=0"）。
    float3 sunDir    = float3(0.0f, 1.0f, 0.0f);
    float  turbidity = 0.0f;
    const bool found = he::GetPhysicalSkySun(world, sunDir, turbidity);
    out.atmosphere = float4(sunDir, turbidity);

    // 物理天空的**整份参数**（`SkyboxPass` 要用 intensity/groundAlbedo/sunIntensity 等）：
    // 口径与上面完全一致 —— 取**第一个启用**的组件（`GetPhysicalSkySun` 的同一规则），
    // 找不到时整份复位为"关闭"（否则上一帧的参数会残留在快照里）。
    out.physicalSky = SnapshotPhysicalSky{};
    world.ForEach<he::PhysicalSkyComponent>([&](he::Entity, he::PhysicalSkyComponent& ps) {
        if (!ps.enabled || out.physicalSky.enabled) return;   // 只认第一个启用的
        out.physicalSky.enabled      = true;
        out.physicalSky.sunDirection = ps.sunDirection;
        out.physicalSky.turbidity    = ps.turbidity;
        out.physicalSky.groundAlbedo = ps.groundAlbedo;
        out.physicalSky.intensity    = ps.intensity;
        out.physicalSky.sunIntensity = ps.sunIntensity;
    });
    return found;
}

u32 SceneSnapshotBuilder::BuildMaterials(he::World& world, FrameSceneSnapshot& out) {
    out.materials.clear();

    // 按 materialID 去重（同一 materialID 的多个物体共享一份材质数据），逻辑与原
    // `ForwardPipeline::UploadMaterialBindless` 的收集段逐条一致（含"收集哪些组件类型"）。
    std::unordered_map<u32, GPUMaterialData> uniqueMat;
    auto collect = [&](he::MeshComponent& m) {
        if (uniqueMat.count(m.materialID)) return;          // 已收集过该材质，跳过
        GPUMaterialData g;
        FillMaterialData(g, MakePBRMaterial(m));            // 映射走唯一实现（E-3①）
        uniqueMat[m.materialID] = g;
    };
    world.ForEach<he::MeshComponent>([&](he::Entity, he::MeshComponent& m) { collect(m); });
    world.ForEach<he::CubeComponent>([&](he::Entity, he::CubeComponent& c) { collect(c); });
    world.ForEach<he::SphereComponent>([&](he::Entity, he::SphereComponent& s) { collect(s); });
    world.ForEach<he::InstancedMeshComponent>([&](he::Entity, he::InstancedMeshComponent& im) { collect(im); });
    world.ForEach<he::SkeletalMeshComponent>([&](he::Entity, he::SkeletalMeshComponent& sm) { collect(sm); });
    world.ForEach<he::SplineMeshComponent>([&](he::Entity, he::SplineMeshComponent& spl) { collect(spl); });

    if (uniqueMat.empty()) return 0u;                       // 场景无材质：留空数组（上传侧据此跳过）

    // materialID 是纹理基索引（每材质占 4 个纹理槽），材质数据索引 = materialID >> 2
    u32 maxSlot = 0;
    for (const auto& kv : uniqueMat) maxSlot = std::max(maxSlot, kv.first >> 2);
    // 空槽位填默认 GPUMaterialData{}（值初始化 = 全 0），保证 buffer 内索引与 materialID>>2 对齐
    out.materials.assign(static_cast<usize>(maxSlot) + 1u, GPUMaterialData{});
    for (const auto& kv : uniqueMat) out.materials[kv.first >> 2] = kv.second;
    return static_cast<u32>(out.materials.size());
}

u32 SceneSnapshotBuilder::BuildDecals(he::World& world, he::SceneGraph& sg, FrameSceneSnapshot& out) {
    out.decals.clear();
    world.ForEach<he::DecalComponent>([&](he::Entity e, he::DecalComponent& d) {
        SnapshotDecal item;
        item.worldMatrix          = sg.GetWorldMatrix(e);   // 原 DecalPass.cpp:249
        item.size                 = d.size;
        item.projectionDepth      = d.projectionDepth;
        item.rotation             = d.rotation;
        item.baseColorFactor      = d.baseColorFactor;
        item.metallicFactor       = d.metallicFactor;
        item.roughnessFactor      = d.roughnessFactor;
        item.opacity              = d.opacity;
        item.materialID           = d.materialID;
        item.hasBaseColorTexture  = !d.baseColorTexture.empty();
        out.decals.push_back(item);
    });
    return static_cast<u32>(out.decals.size());
}

bool SceneSnapshotBuilder::BuildSkybox(he::World& world, FrameSceneSnapshot& out) {
    out.skybox = SnapshotSkybox{};          // 逐帧复位：上一帧的天空盒不得残留
    // 与原先帧图里的 `world.ForEach<SkyboxComponent>` 循环逐条一致（逐个赋值 ⇒ 后者覆盖前者）。
    world.ForEach<he::SkyboxComponent>([&](he::Entity, he::SkyboxComponent& sc) {
        if (sc.enabled && sc.GetCubemap()) {
            out.skybox.cubemap = sc.GetCubemap();
            out.skybox.sampler = sc.GetCubemapSampler();
            out.skybox.enabled = true;
            out.skybox.intensity = sc.intensity;   // 第③段：`SkyboxPass` 的 push constant 要用它
        }
    });
    return out.skybox.enabled;
}

u32 SceneSnapshotBuilder::RegisterMeshes(he::World& world, MeshRegistry& registry) {    u32 count = 0;
    // 登记一个网格组件：缓冲只借指针（所有权在组件）、回填注册表索引。
    // `instanced=true` 的形态（实例化/骨骼）顶点由各自专用路径提供，绘制循环会跳过普通绘制。
    auto add = [&](auto& comp, bool instanced, rhi::IRHIBuffer* skinBuffer = nullptr) {
        if (comp.GetIndexCount() == 0u) return;                // 与收集口径一致：无索引不登记
        MeshRegistryEntry entry;
        entry.vertexBuffer     = comp.GetVertexBuffer().get();
        entry.indexBuffer      = comp.GetIndexBuffer().get();
        entry.skinMatrixBuffer = skinBuffer;                   // 仅骨骼网格非空（骨骼上传的写入目标）
        entry.indexCount       = comp.GetIndexCount();
        entry.materialID       = comp.materialID;
        entry.instanced        = instanced;
        comp.meshIndex         = registry.Register(&comp, entry);
        ++count;
    };

    // 遍历顺序与 `BuildObjects` / `SceneRenderer::Prepare` 的收集顺序无关（注册表按组件地址为键），
    // 因此这里按"基类 → 各派生类型"逐个列出即可；`World::ForEach<T>` 只匹配**精确**类型，故要列全。
    world.ForEach<MeshComponent>([&](he::Entity, MeshComponent& c) { add(c, false); });
    world.ForEach<CubeComponent>([&](he::Entity, CubeComponent& c) { add(c, false); });
    world.ForEach<SphereComponent>([&](he::Entity, SphereComponent& c) { add(c, false); });
    world.ForEach<BillboardComponent>([&](he::Entity, BillboardComponent& c) { add(c, false); });
    world.ForEach<TextRenderComponent>([&](he::Entity, TextRenderComponent& c) { add(c, false); });
    world.ForEach<DecalComponent>([&](he::Entity, DecalComponent& c) { add(c, false); });
    world.ForEach<SplineMeshComponent>([&](he::Entity, SplineMeshComponent& c) { add(c, false); });
    world.ForEach<InstancedMeshComponent>([&](he::Entity, InstancedMeshComponent& c) { add(c, true); });
    // 骨骼网格：除上述字段外还要登记**骨骼缓冲**（骨骼上传的写入目标，会重建 ⇒ 每帧刷新正为此刻）
    world.ForEach<SkeletalMeshComponent>([&](he::Entity, SkeletalMeshComponent& sm) {
        add(sm, true, sm.boneBuffer.get());
    });
    return count;
}

u32 SceneSnapshotBuilder::BuildInstances(he::World& world, FrameSceneSnapshot& out) {
    out.instances.clear();
    out.instanceTransforms.clear();

    world.ForEach<InstancedMeshComponent>([&](he::Entity e, InstancedMeshComponent& im) {
        SnapshotInstance item;
        // `meshIndex` 由 `RegisterMeshes` 回填（调用方必须先注册）；0 = 未注册，
        // 消费侧 `MeshRegistry::Find(0)` 返回空并跳过 —— 可见化，而不是指错资源。
        item.meshIndex        = im.meshIndex;
        item.transformOffset  = static_cast<u32>(out.instanceTransforms.size());
        item.transformCount   = static_cast<u32>(im.instanceTransforms.size());
        item.transformVersion = im.instanceTransformVersion;
        item.enableFrustumCull = im.enableFrustumCull;
        // 局部包围盒（逐实例剔除 shader 的输入）：数据源是网格几何，与实例变换无关，
        // 因此可以按网格烤进快照 —— 渲染侧不必再去问组件。
        const he::AABB lb = im.GetBounds();
        item.localBoundsMin = lb.min;
        item.localBoundsMax = lb.max;
        item.sourceEntity   = e.id;   // 渲染侧状态表识别"索引复用后的新网格"用

        out.instanceTransforms.insert(out.instanceTransforms.end(),
                                      im.instanceTransforms.begin(), im.instanceTransforms.end());
        out.instances.push_back(item);
    });
    return static_cast<u32>(out.instances.size());
}

u32 SceneSnapshotBuilder::BuildShadowLights(he::World& world, he::SceneGraph& sg,
                                            FrameSceneSnapshot& out) {
    out.shadowLights.clear();

    // 四个阴影技术的过滤条件是同一句（`!enabled || !castShadow` 即跳过），故在收集侧一次过滤。
    // `type` 用 `he::LightType` 的枚举值，技术侧按它分流（顺序见头文件说明）。
    auto addDirectional = [&](he::Entity e, he::DirectionalLight& lc) {
        if (!lc.enabled || !lc.castShadow) return;
        SnapshotShadowLight l{};
        l.type               = static_cast<u32>(he::LightType::Directional);
        l.direction          = lc.direction;                 // 不归一化：CSM 侧自己 normalize
        l.shadowBias         = lc.shadowBias;
        l.shadowNormalBias   = lc.shadowNormalBias;
        l.shadowStrength     = lc.shadowStrength;
        l.sourceEntity       = e.id;
        out.shadowLights.push_back(l);
    };
    auto addPoint = [&](he::Entity e, he::PointLight& lc) {
        if (!lc.enabled || !lc.castShadow) return;
        SnapshotShadowLight l{};
        l.type               = static_cast<u32>(he::LightType::Point);
        l.position           = sg.GetWorldPosition(e);
        l.range              = lc.range;
        l.shadowBias         = lc.shadowBias;
        l.shadowNormalBias   = lc.shadowNormalBias;
        l.shadowStrength     = lc.shadowStrength;
        l.sourceEntity       = e.id;
        out.shadowLights.push_back(l);
    };
    auto addSpot = [&](he::Entity e, he::SpotLight& lc) {
        if (!lc.enabled || !lc.castShadow) return;
        SnapshotShadowLight l{};
        l.type               = static_cast<u32>(he::LightType::Spot);
        l.direction          = lc.direction;                 // 不归一化：Spot 侧自己 normalize
        l.position           = sg.GetWorldPosition(e);
        l.range              = lc.range;
        l.outerConeAngle     = lc.outerConeAngle;
        l.shadowBias         = lc.shadowBias;
        l.shadowNormalBias   = lc.shadowNormalBias;
        l.shadowStrength     = lc.shadowStrength;
        l.sourceEntity       = e.id;
        out.shadowLights.push_back(l);
    };
    auto addRect = [&](he::Entity e, he::RectLight& lc) {
        if (!lc.enabled || !lc.castShadow) return;
        SnapshotShadowLight l{};
        l.type               = static_cast<u32>(he::LightType::Rect);
        l.direction          = lc.normal;                    // 面光复用 direction 字段存法线
        l.position           = sg.GetWorldPosition(e);
        l.range              = lc.range;
        l.softness           = lc.softness;
        l.shadowBias         = lc.shadowBias;
        l.shadowNormalBias   = lc.shadowNormalBias;
        l.shadowStrength     = lc.shadowStrength;
        l.sourceEntity       = e.id;
        out.shadowLights.push_back(l);
    };

    world.ForEach<he::DirectionalLight>(addDirectional);
    world.ForEach<he::PointLight>(addPoint);
    world.ForEach<he::SpotLight>(addSpot);
    world.ForEach<he::RectLight>(addRect);
    return static_cast<u32>(out.shadowLights.size());
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
