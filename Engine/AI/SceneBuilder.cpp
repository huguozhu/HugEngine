#include "AI/SceneBuilder.h"

#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/LightComponent.h"
#include "Scene/PhysicalSkyComponent.h"
#include "Core/Log.h"

#include "nlohmann/json.hpp"

using nlohmann::json;

namespace he::ai {

// ============================================================
// 解析辅助（JSON → 引擎类型）
// ============================================================

// 解析 JSON 数组为 float3（默认 def，缺省/非法时返回）
static float3 ParseVec3(const json& j, const float3& def = float3(0.0f)) {
    if (!j.is_array() || j.size() < 3) return def;
    return float3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}

// 解析 JSON 数组为 float4（第 4 分量默认 1.0，用于 baseColor）
static float4 ParseVec4(const json& j, const float4& def = float4(1.0f)) {
    if (!j.is_array() || j.size() < 3) return def;
    float w = j.size() >= 4 ? j[3].get<float>() : 1.0f;
    return float4(j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), w);
}

// 从 JSON 读取可选字符串字段（缺省返回 def）
static String GetString(const json& j, const char* key, const char* def) {
    if (j.contains(key) && j[key].is_string()) return j[key].get<String>();
    return def;
}

// 设置网格材质参数（MeshComponent 基类字段，Cube/Sphere 共用）
static void SetMeshMaterial(MeshComponent* mesh, const json& comp) {
    if (comp.contains("baseColor")) mesh->baseColorFactor = ParseVec4(comp["baseColor"]);
    if (comp.contains("metallic"))  mesh->metallicFactor  = comp["metallic"].get<float>();
    if (comp.contains("roughness")) mesh->roughnessFactor = comp["roughness"].get<float>();
    if (comp.contains("emissive"))  mesh->emissiveFactor  = ParseVec3(comp["emissive"]);
}

// ============================================================
// BuildScene — 场景 JSON 解释器
// ============================================================

SceneBuildResult BuildScene(World& world, SceneGraph& sg, const String& sceneJson) {
    SceneBuildResult result;

    // 1. 解析 JSON（非法 JSON 直接失败，不产生任何实体）
    json root;
    try {
        root = json::parse(sceneJson);
    } catch (const std::exception& e) {
        result.error = String("JSON 解析失败: ") + e.what();
        return result;
    }

    // 2. 结构校验：必须有 entities 数组
    if (!root.contains("entities") || !root["entities"].is_array()) {
        result.error = "JSON 缺少 entities 数组";
        return result;
    }

    // 3. 逐实体创建：Transform + 组件（硬编码组件映射）
    for (const auto& ent : root["entities"]) {
        Entity e = world.CreateEntity(GetString(ent, "name", "Entity"));

        // --- Transform（每个实体必备，缺省默认值）---
        auto* xform = world.AddComponent<TransformComponent>(e);
        if (ent.contains("transform")) {
            const auto& t = ent["transform"];
            if (t.contains("position")) xform->position = ParseVec3(t["position"]);
            if (t.contains("scale"))    xform->scale    = ParseVec3(t["scale"], float3(1.0f));
        }

        // --- 组件（硬编码映射：Cube/Sphere/两种光源/物理天空）---
        if (ent.contains("components") && ent["components"].is_array()) {
            for (const auto& comp : ent["components"]) {
                String type = GetString(comp, "type", "");

                if (type == "Cube") {
                    auto* c = world.AddComponent<CubeComponent>(e);
                    if (comp.contains("halfExtent")) c->halfExtent = comp["halfExtent"].get<float>();
                    SetMeshMaterial(static_cast<MeshComponent*>(c), comp);
                    c->OnCreate();  // 用新 halfExtent 重建几何
                }
                else if (type == "Sphere") {
                    auto* s = world.AddComponent<SphereComponent>(e);
                    if (comp.contains("radius"))       s->radius       = comp["radius"].get<float>();
                    if (comp.contains("segmentCount")) s->segmentCount = comp["segmentCount"].get<u32>();
                    if (comp.contains("ringCount"))    s->ringCount    = comp["ringCount"].get<u32>();
                    SetMeshMaterial(static_cast<MeshComponent*>(s), comp);
                    s->OnCreate();
                }
                else if (type == "DirectionalLight") {
                    auto* l = world.AddComponent<DirectionalLight>(e);
                    if (comp.contains("direction"))  l->direction  = ParseVec3(comp["direction"], l->direction);
                    if (comp.contains("color"))      l->color      = ParseVec3(comp["color"], l->color);
                    if (comp.contains("intensity"))  l->intensity  = comp["intensity"].get<float>();
                    if (comp.contains("castShadow")) l->castShadow = comp["castShadow"].get<bool>();
                }
                else if (type == "PointLight") {
                    auto* l = world.AddComponent<PointLight>(e);
                    if (comp.contains("color"))     l->color     = ParseVec3(comp["color"], l->color);
                    if (comp.contains("intensity")) l->intensity = comp["intensity"].get<float>();
                    if (comp.contains("range"))     l->range     = comp["range"].get<float>();
                }
                else if (type == "PhysicalSky") {
                    auto* s = world.AddComponent<PhysicalSkyComponent>(e);
                    if (comp.contains("sunDirection")) s->sunDirection = ParseVec3(comp["sunDirection"], s->sunDirection);
                    if (comp.contains("turbidity"))    s->turbidity    = comp["turbidity"].get<float>();
                    if (comp.contains("intensity"))    s->intensity    = comp["intensity"].get<float>();
                    s->OnCreate();  // 归一化 sunDirection
                }
                else {
                    // 未知组件类型：跳过并告警（容错，不影响其余实体）
                    HE_CORE_WARN("[SceneBuilder] 未知组件类型，已跳过: {}", type);
                }
            }
        }

        // 挂到场景图根节点（与 glTF 导入器一致）
        sg.SetParent(e, Entity{kInvalidEntity});
        result.entities.push_back(e);
    }

    result.success = true;
    return result;
}

} // namespace he::ai
