// ============================================================
// TypeRegistry — 全局类型注册表实现
// ============================================================
#include "Reflect/TypeInfo.h"

namespace he::reflect {

TypeRegistry& TypeRegistry::Instance() {
    static TypeRegistry s_Registry;
    return s_Registry;
}

void TypeRegistry::RegisterClass(const ClassInfo* info) {
    if (!info) return;
    m_ClassMap[info->typeHash] = info;
}

const ClassInfo* TypeRegistry::FindClass(StringView name) const {
    u64 hash = HashString(name);
    return FindClassByHash(hash);
}

const ClassInfo* TypeRegistry::FindClassByHash(u64 hash) const {
    auto it = m_ClassMap.find(hash);
    return (it != m_ClassMap.end()) ? it->second : nullptr;
}

void TypeRegistry::ForEachClass(std::function<void(const ClassInfo&)> callback) const {
    for (auto& [hash, info] : m_ClassMap) {
        if (info) callback(*info);
    }
}

} // namespace he::reflect
