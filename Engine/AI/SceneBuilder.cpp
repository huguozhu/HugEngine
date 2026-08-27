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
//
// LLM 输出不可靠：字段可能缺失、类型不符、越界。
// 所有读取都做类型检查，失败时返回默认值降级，绝不抛异常。
// ============================================================

// 解析 JSON 数组为 float3（默认 def，缺省/非法时返回；元素逐个容错）
static float3 ParseVec3(const json& j, const float3& def = float3(0.0f)) {
    if (!j.is_array() || j.size() < 3) return def;
    float3 r = def;
    if (j[0].is_number()) r.x = j[0].get<float>();
    if (j[1].is_number()) r.y = j[1].get<float>();
    if (j[2].is_number()) r.z = j[2].get<float>();
    return r;
}

// 解析 JSON 数组为 float4（第 4 分量默认 1.0，用于 baseColor；元素逐个容错）
static float4 ParseVec4(const json& j, const float4& def = float4(1.0f)) {
    if (!j.is_array() || j.size() < 3) return def;
    float4 r = def;
    if (j[0].is_number()) r.x = j[0].get<float>();
    if (j[1].is_number()) r.y = j[1].get<float>();
    if (j[2].is_number()) r.z = j[2].get<float>();
    if (j.size() >= 4 && j[3].is_number()) r.w = j[3].get<float>();
    return r;
}

// 从 JSON 读取可选字符串字段（缺省返回 def）
static String GetString(const json& j, const char* key, const char* def) {
    if (j.contains(key) && j[key].is_string()) return j[key].get<String>();
    return def;
}

// 安全读取浮点字段：缺省/类型不符时返回默认值（LLM 可能给数组/字符串）
static float GetFloatField(const json& j, const char* key, float def) {
    if (j.contains(key) && j[key].is_number()) return j[key].get<float>();
    return def;
}

// 安全读取无符号整数字段：缺省/类型不符时返回默认值
static u32 GetU32Field(const json& j, const char* key, u32 def) {
    if (j.contains(key) && j[key].is_number_integer()) return j[key].get<u32>();
    return def;
}

// 安全读取布尔字段：缺省/类型不符时返回默认值
static bool GetBoolField(const json& j, const char* key, bool def) {
    if (j.contains(key) && j[key].is_boolean()) return j[key].get<bool>();
    return def;
}

// 设置网格材质参数（MeshComponent 基类字段，Cube/Sphere 共用，全部安全读取）
static void SetMeshMaterial(MeshComponent* mesh, const json& comp) {
    if (comp.contains("baseColor")) mesh->baseColorFactor = ParseVec4(comp["baseColor"]);
    mesh->metallicFactor  = GetFloatField(comp, "metallic",  mesh->metallicFactor);
    mesh->roughnessFactor = GetFloatField(comp, "roughness", mesh->roughnessFactor);
    if (comp.contains("emissive")) mesh->emissiveFactor = ParseVec3(comp["emissive"]);
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
                    c->halfExtent = GetFloatField(comp, "halfExtent", c->halfExtent);
                    SetMeshMaterial(static_cast<MeshComponent*>(c), comp);
                    c->OnCreate();  // 用新 halfExtent 重建几何
                }
                else if (type == "Sphere") {
                    auto* s = world.AddComponent<SphereComponent>(e);
                    s->radius       = GetFloatField(comp, "radius", s->radius);
                    s->segmentCount = GetU32Field(comp, "segmentCount", s->segmentCount);
                    s->ringCount    = GetU32Field(comp, "ringCount", s->ringCount);
                    SetMeshMaterial(static_cast<MeshComponent*>(s), comp);
                    s->OnCreate();
                }
                else if (type == "DirectionalLight") {
                    auto* l = world.AddComponent<DirectionalLight>(e);
                    if (comp.contains("direction")) l->direction = ParseVec3(comp["direction"], l->direction);
                    if (comp.contains("color"))     l->color     = ParseVec3(comp["color"], l->color);
                    l->intensity  = GetFloatField(comp, "intensity", l->intensity);
                    l->castShadow = GetBoolField(comp, "castShadow", l->castShadow);
                }
                else if (type == "PointLight") {
                    auto* l = world.AddComponent<PointLight>(e);
                    if (comp.contains("color")) l->color = ParseVec3(comp["color"], l->color);
                    l->intensity = GetFloatField(comp, "intensity", l->intensity);
                    l->range     = GetFloatField(comp, "range", l->range);
                }
                else if (type == "PhysicalSky") {
                    auto* s = world.AddComponent<PhysicalSkyComponent>(e);
                    if (comp.contains("sunDirection")) s->sunDirection = ParseVec3(comp["sunDirection"], s->sunDirection);
                    s->turbidity = GetFloatField(comp, "turbidity", s->turbidity);
                    s->intensity = GetFloatField(comp, "intensity", s->intensity);
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
