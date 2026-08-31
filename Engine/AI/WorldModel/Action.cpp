#include "AI/WorldModel/Action.h"

#include "AI/SceneBuilder.h"
#include "Reflect/ReflectionAPI.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/Transform.h"
#include "Scene/Component.h"
#include "Editor/Command.h"
#include "Core/Log.h"

#include "nlohmann/json.hpp"

#include <utility>

using nlohmann::json;

namespace he::ai {

namespace {

// ============================================================
// JSON 解析辅助（与 SceneBuilder 同风格，小段复制避免跨文件耦合）
// ============================================================

// 解析 JSON 数组为 float3（缺省/非法返回 def）
float3 ParseVec3(const json& j, const float3& def) {
    if (!j.is_array() || j.size() < 3) return def;
    float3 r = def;
    if (j[0].is_number()) r.x = j[0].get<float>();
    if (j[1].is_number()) r.y = j[1].get<float>();
    if (j[2].is_number()) r.z = j[2].get<float>();
    return r;
}

} // namespace

// ============================================================
// SpawnEntityCommand — 新建实体命令（可撤销）
// 放在匿名命名空间之外，供 CompileAction 直接构造。
// ============================================================

/// 新建实体命令：Execute 用 SceneBuilder 装配实体，Undo 销毁
class SpawnEntityCommand : public he::Command {
public:
    SpawnEntityCommand(he::World& world, he::SceneGraph& sg, String entityJson)
        : m_World(world), m_SG(sg), m_EntityJson(std::move(entityJson)) {}

    void Execute() override {
        try {
            // 单个实体规格 → 包装成 entities 数组交给 BuildScene（复用装配）
            json scene = json::object();
            scene["entities"] = json::array({json::parse(m_EntityJson)});
            he::ai::SceneBuildResult r = he::ai::BuildScene(m_World, m_SG, scene.dump());
            if (r.success) m_Created = std::move(r.entities);
        } catch (const std::exception&) {
            HE_CORE_WARN("[Action] SpawnEntityCommand::Execute 解析失败");
        }
    }

    void Undo() override {
        // 逆序销毁本次创建的实体
        for (auto it = m_Created.rbegin(); it != m_Created.rend(); ++it)
            m_World.DestroyEntity(*it);
        m_Created.clear();
    }

    String GetDescription() const override { return "AI SpawnEntity"; }

private:
    he::World&        m_World;
    he::SceneGraph&   m_SG;
    String            m_EntityJson;
    TArray<he::Entity> m_Created;   // 回滚用
};

// ============================================================
// 反射属性值读写（支持 MVP 标量/向量/字符串类型）
// ============================================================

// 读属性值 → JSON
bool GetValueJson(const he::reflect::PropertyInfo& p, void* ptr, json& out) {
    const StringView t = p.typeName;
    if (t == "float")        { out = *(const float*)ptr;  return true; }
    if (t == "double")       { out = *(const double*)ptr; return true; }
    if (t == "int" || t == "i32") { out = *(const int*)ptr;  return true; }
    if (t == "u32")          { out = *(const u32*)ptr;     return true; }
    if (t == "bool")         { out = *(const bool*)ptr;    return true; }
    if (t == "float3") {
        const float* f = (const float*)ptr;
        out = json::array({f[0], f[1], f[2]});
        return true;
    }
    if (t == "float4") {
        const float* f = (const float*)ptr;
        out = json::array({f[0], f[1], f[2], f[3]});
        return true;
    }
    if (t == "String" || t == "std::string") { out = *(const String*)ptr; return true; }
    return false;   // 未知类型不支持
}

// 写 JSON 值 → 属性
bool SetValueFromJson(const he::reflect::PropertyInfo& p, void* ptr, const json& v) {
    const StringView t = p.typeName;
    if (t == "float" && v.is_number())        { *(float*)ptr = v.get<float>();  return true; }
    if (t == "double" && v.is_number())       { *(double*)ptr = v.get<double>(); return true; }
    if (t == "int" && v.is_number_integer())  { *(int*)ptr = v.get<int>();      return true; }
    if (t == "u32" && v.is_number_integer())  { *(u32*)ptr = v.get<u32>();      return true; }
    if (t == "bool" && v.is_boolean())        { *(bool*)ptr = v.get<bool>();    return true; }
    if (t == "float3" && v.is_array() && v.size() >= 3) {
        float* f = (float*)ptr;
        for (int i = 0; i < 3; ++i)
            if (v[i].is_number()) f[i] = v[i].get<float>();
        return true;
    }
    if (t == "float4" && v.is_array() && v.size() >= 4) {
        float* f = (float*)ptr;
        for (int i = 0; i < 4; ++i)
            if (v[i].is_number()) f[i] = v[i].get<float>();
        return true;
    }
    if (t == "String" && v.is_string())       { *(String*)ptr = v.get<String>(); return true; }
    return false;
}

// 按组件类型名在实体上找组件（返回组件基类指针与 ClassInfo）
Component* FindComponentByType(World& world, Entity e, StringView typeName) {
    Component* found = nullptr;
    world.ForEachComponent(e, [&](Component* comp) {
        if (found) return;
        const he::reflect::ClassInfo* cls = comp->GetClass();
        if (cls && cls->name == typeName) found = comp;
    });
    return found;
}

// ============================================================
// CompileAction — Action → he::Command
// ============================================================

std::unique_ptr<he::Command> CompileAction(World& world, SceneGraph& sg, const Action& a) {
    // 解析参数（非法 JSON → nullptr）
    json args;
    try {
        args = json::parse(a.argsJson);
    } catch (const std::exception&) {
        return nullptr;
    }

    // ---------- SpawnEntity：新建实体（复用 SceneBuilder 装配，可撤销） ----------
    if (a.op == "SpawnEntity") {
        // argsJson = 单个实体规格（含 transform/components）
        return std::make_unique<SpawnEntityCommand>(world, sg, a.argsJson);
    }

    // ---------- SetTransform：修改 TransformComponent ----------
    if (a.op == "SetTransform") {
        auto* xform = world.GetComponent<TransformComponent>(Entity{a.targetEntity});
        if (!xform) {
            HE_CORE_WARN("[Action] SetTransform 目标实体 {} 无 TransformComponent", a.targetEntity);
            return nullptr;
        }
        // 捕获旧值 / 解析新值（缺省保持旧值）
        float3 oldPos = xform->position, oldScale = xform->scale;
        float3 newPos = args.contains("position") ? ParseVec3(args["position"], oldPos) : oldPos;
        float3 newScale = args.contains("scale") ? ParseVec3(args["scale"], oldScale) : oldScale;
        return std::make_unique<he::PropertyChangeCommand>(
            "AI SetTransform",
            [xform, oldPos, oldScale] { xform->position = oldPos; xform->scale = oldScale; },
            [xform, newPos, newScale] { xform->position = newPos; xform->scale = newScale; });
    }

    // ---------- SetProperty：经反射修改组件属性 ----------
    if (a.op == "SetProperty") {
        StringView compType = args.value("component", StringView{});
        StringView propName = args.value("property", StringView{});
        auto* comp = FindComponentByType(world, Entity{a.targetEntity}, compType);
        if (!comp) {
            HE_CORE_WARN("[Action] SetProperty 目标实体 {} 无组件 {}", a.targetEntity, compType);
            return nullptr;
        }
        const he::reflect::ClassInfo* cls = comp->GetClass();
        const he::reflect::PropertyInfo* prop = cls ? cls->FindProperty(propName) : nullptr;
        if (!prop) {
            HE_CORE_WARN("[Action] SetProperty 组件 {} 无属性 {}", compType, propName);
            return nullptr;
        }
        void* ptr = reinterpret_cast<char*>(comp) + prop->offset;

        // 新值
        if (!args.contains("value")) return nullptr;
        // 旧值（Undo 用）
        json oldVal;
        if (!GetValueJson(*prop, ptr, oldVal)) return nullptr;
        return std::make_unique<he::PropertyChangeCommand>(
            String("AI SetProperty ") + String(propName),
            [comp, prop, oldVal] { SetValueFromJson(*prop, reinterpret_cast<char*>(comp) + prop->offset, oldVal); },
            [comp, prop, args]   { SetValueFromJson(*prop, reinterpret_cast<char*>(comp) + prop->offset, args["value"]); });
    }

    // 其他 op（CallTool 等）暂不支持
    HE_CORE_WARN("[Action] 暂不支持的动作 op: {}", a.op);
    return nullptr;
}

} // namespace he::ai
