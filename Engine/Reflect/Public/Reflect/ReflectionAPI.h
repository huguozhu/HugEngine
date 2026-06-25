#pragma once

#include "Core/Types.h"

#include <string_view>
#include <vector>
#include <unordered_map>
#include <functional>

// ============================================================
// ReflectionAPI.h — 运行时反射稳定接口
//
// 消费者（序列化/编辑器/网络）只依赖此文件。
// 后端通过填充 ClassInfo / PropertyInfo 提供数据，
// 与宏后端或未来 C++26 consteval 后端解耦。
//
// 切换后端只需替换 ReflectionMacros.h，本文件不变。
// ============================================================

namespace he::reflect {

// ============================================================
// 属性元数据
// ============================================================

struct AttributeValue {
    StringView key;
    String     value;
};

enum PropertyFlags : u32 {
    PF_None         = 0,
    PF_ReadOnly     = 1 << 0,
    PF_Hidden       = 1 << 1,
    PF_Deprecated   = 1 << 2,
    PF_Serializable = 1 << 3,
    PF_Replicated   = 1 << 4,
};

struct PropertyInfo {
    StringView  name;       // 成员名
    usize       offset;     // 字节偏移
    usize       size;       // sizeof(成员)
    StringView  typeName;   // 类型名字符串
    u32         flags = PF_None;

    std::vector<std::pair<StringView, String>> attributes;

    StringView GetAttribute(StringView key) const {
        for (auto& [k, v] : attributes)
            if (k == key) return v;
        return {};
    }
};

// ============================================================
// 类型元数据
// ============================================================

struct ClassInfo {
    StringView  name;
    usize       size;
    u64         typeHash;       // FNV-1a 64-bit
    ClassInfo*  parent = nullptr;

    std::function<void*(void)>  factory;
    std::vector<PropertyInfo>   properties;

    const PropertyInfo* FindProperty(StringView name) const {
        for (auto& p : properties)
            if (p.name == name) return &p;
        return nullptr;
    }
};

// FNV-1a 64-bit 哈希（编译期 + 运行期可用）
constexpr u64 HashString(StringView str) {
    u64 hash = 14695981039346656037ull;
    for (char c : str) {
        hash ^= static_cast<u64>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

// ============================================================
// 全局类型注册表
// ============================================================

class TypeRegistry {
public:
    static TypeRegistry& Instance();

    void RegisterClass(const ClassInfo* info);

    const ClassInfo* FindClass(StringView name) const;
    const ClassInfo* FindClassByHash(u64 hash) const;

    void ForEachClass(std::function<void(const ClassInfo&)> callback) const;

private:
    std::unordered_map<u64, const ClassInfo*> m_ClassMap;
};

// ============================================================
// 反射消费者工具（后端无关）
// ============================================================

// 获取成员指针（通过偏移量）
template<typename T, typename MemberType>
MemberType* GetMemberPtr(T* obj, const PropertyInfo& prop) {
    return reinterpret_cast<MemberType*>(
        reinterpret_cast<char*>(obj) + prop.offset);
}

// 遍历对象所有反射属性
template<typename T>
void ForEachProperty(T* obj, std::function<void(const PropertyInfo&, void*)> callback) {
    const ClassInfo* cls = obj->GetClass();
    if (!cls) return;
    for (auto& prop : cls->properties) {
        void* ptr = reinterpret_cast<char*>(obj) + prop.offset;
        callback(prop, ptr);
    }
}

} // namespace he::reflect
