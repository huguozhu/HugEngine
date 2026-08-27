#include "AI/WorldModel/WorldModel.h"

#include "Reflect/ReflectionAPI.h"
#include "Reflect/Attribute.h"
#include "Scene/World.h"
#include "Scene/Component.h"
#include "Core/Log.h"

#include "nlohmann/json.hpp"

using nlohmann::json;

namespace he::ai {

namespace {

// ============================================================
// 序列化辅助
// ============================================================

// 去掉类型名的 "he::" 前缀（LLM 场景 JSON 里组件类型不带命名空间）
StringView StripNamespace(StringView name) {
    constexpr StringView kPrefix = "he::";
    if (name.starts_with(kPrefix)) return name.substr(kPrefix.size());
    return name;
}

// 按反射 typeName 把属性值序列化成 JSON。
// LLM/序列化兜底：不认识的类型输出 null 并告警，不抛异常。
bool SerializeValue(const he::reflect::PropertyInfo& prop, void* ptr, json& out) {
    const StringView t = prop.typeName;
    if (t == "float")      { out = *(const float*)ptr;           return true; }
    if (t == "double")     { out = *(const double*)ptr;          return true; }
    if (t == "int" || t == "int32" || t == "i32") { out = *(const int*)ptr; return true; }
    if (t == "u32" || t == "uint32") { out = *(const u32*)ptr;    return true; }
    if (t == "u8")         { out = *(const u8*)ptr;               return true; }
    if (t == "bool")       { out = *(const bool*)ptr;             return true; }
    if (t == "float2") {
        const float* f = (const float*)ptr;
        out = json::array({f[0], f[1]});
        return true;
    }
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
    if (t == "quat") {
        const float* f = (const float*)ptr;
        out = json::array({f[0], f[1], f[2], f[3]});
        return true;
    }
    if (t == "String" || t == "std::string") {
        out = *(const String*)ptr;
        return true;
    }
    // 未知类型：跳过，避免序列化错误数据
    HE_CORE_WARN("[WorldModel] 暂不支持序列化类型: {}", t);
    return false;
}

// 判断属性是否对 AI 可见（带 HE_ATTR_AI_VISIBLE 注解）
bool IsAiVisible(const he::reflect::PropertyInfo& prop) {
    return prop.GetAttribute(he::reflect::AttrKey::AiVisible) == "1";
}

} // namespace

// ============================================================
// Snapshot — World → 语义快照
// ============================================================

String WorldModel::Snapshot(World& world, const ObservationFilter& filter) const {
    json root;
    root["entities"] = json::array();

    // 遍历所有存活实体
    world.ForEachEntity([&](Entity e) {
        // 目标实体过滤（0 = 全部）
        if (filter.targetEntity != 0 && e.id != filter.targetEntity) return;

        json ent;
        ent["id"] = e.id;

        ent["components"] = json::array();
        // 遍历该实体的全部组件，按 AI_VISIBLE 注解过滤导出
        world.ForEachComponent(e, [&](Component* comp) {
            const he::reflect::ClassInfo* cls = comp->GetClass();
            if (!cls) return;

            // 组件类型过滤（空 = 全部）
            StringView typeName = StripNamespace(cls->name);
            if (!filter.componentTypes.empty()) {
                bool matched = false;
                for (auto& t : filter.componentTypes) {
                    if (typeName == t) { matched = true; break; }
                }
                if (!matched) return;
            }

            // 该类型没有任何 AI_VISIBLE 属性则整组件跳过
            bool anyVisible = false;
            for (auto& p : cls->properties)
                if (IsAiVisible(p)) { anyVisible = true; break; }
            if (!anyVisible) return;

            json c;
            c["type"] = String(typeName);
            c["fields"] = json::object();
            for (auto& p : cls->properties) {
                if (!IsAiVisible(p)) continue;   // 只导出 AI_VISIBLE 属性
                void* ptr = reinterpret_cast<char*>(comp) + p.offset;
                json val;
                if (SerializeValue(p, ptr, val))
                    c["fields"][String(p.name)] = val;
            }
            ent["components"].push_back(c);
        });

        // 没有可见组件的实体不进快照
        if (!ent["components"].empty())
            root["entities"].push_back(ent);
    });

    return root.dump();
}

// ============================================================
// TypeSchema — 反射驱动组件词汇表
// ============================================================

String WorldModel::TypeSchema() const {
    json root;
    root["component_types"] = json::array();

    he::reflect::TypeRegistry::Instance().ForEachClass([&](const he::reflect::ClassInfo& cls) {
        // 只输出含 AI_VISIBLE 属性的类型（即 AI 可观察/操作的类型）
        bool anyVisible = false;
        for (auto& p : cls.properties)
            if (IsAiVisible(p)) { anyVisible = true; break; }
        if (!anyVisible) return;

        json t;
        t["type"] = String(StripNamespace(cls.name));
        t["fields"] = json::array();
        for (auto& p : cls.properties) {
            if (!IsAiVisible(p)) continue;
            json f;
            f["name"] = String(p.name);
            f["type"] = String(p.typeName);
            // 可写性（HE_ATTR_AI_WRITABLE）
            f["writable"] = p.GetAttribute(he::reflect::AttrKey::AiWritable) == "1";
            // 自然语言说明（HE_ATTR_AI_DESCRIPTION）
            StringView desc = p.GetAttribute(he::reflect::AttrKey::AiDescription);
            if (!desc.empty()) f["description"] = String(desc);
            t["fields"].push_back(f);
        }
        root["component_types"].push_back(t);
    });

    return root.dump();
}

} // namespace he::ai
