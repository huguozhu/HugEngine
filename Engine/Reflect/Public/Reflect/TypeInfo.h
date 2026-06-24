#pragma once

#include "Core/Types.h"

#include <string_view>
#include <vector>
#include <unordered_map>
#include <functional>

// ============================================================
// 运行时类型信息 — 基于宏注册的反射系统
//
// 用法示例:
//   class MyClass {
//       HE_CLASS()
//   public:
//       float health = 100.0f;
//       String name;
//   };
//
//   // 在 .cpp 中注册:
//   HE_BEGIN_REGISTER(MyClass)
//       HE_REGISTER_PROPERTY(float, health)
//           HE_ATTR(Category, "Stats")
//           HE_ATTR(Range, 0.0f, 100.0f)
//       HE_REGISTER_PROPERTY(String, name)
//           HE_ATTR(Category, "Info")
//   HE_END_REGISTER()
// ============================================================

namespace he::reflect {

// --- 前向声明 ---
struct PropertyInfo;
struct ClassInfo;

// --- 属性值（类型擦除的属性参数） ---
struct AttributeValue {
    StringView key;
    String     value;     // 简单字符串存储，按需解析
};

// --- 属性描述（一个反射成员的元数据） ---
struct PropertyInfo {
    StringView  name;           // C++ 成员名
    usize       offset;         // 在所属类中的字节偏移
    usize       size;           // 成员大小（字节）
    StringView  typeName;       // 类型名（编译期字符串）
    u32         flags = 0;      // 属性标志位

    // 属性列表
    std::vector<std::pair<StringView, String>> attributes;

    // 获取属性的字符串值，不存在返回空
    StringView GetAttribute(StringView key) const {
        for (auto& [k, v] : attributes)
            if (k == key) return v;
        return {};
    }
};

// --- 属性标志位 ---
enum PropertyFlags : u32 {
    PF_None         = 0,
    PF_ReadOnly     = 1 << 0,   // 只读
    PF_Hidden       = 1 << 1,   // 编辑器隐藏
    PF_Deprecated   = 1 << 2,   // 已弃用
    PF_Serializable = 1 << 3,   // 参与序列化
    PF_Replicated   = 1 << 4,   // 网络复制
};

// --- 类描述（一个反射类型的完整信息） ---
struct ClassInfo {
    StringView          name;           // 类名
    usize               size;           // sizeof
    u64                 typeHash;       // 类型哈希（FNV-1a）
    ClassInfo*          parent = nullptr; // 父类

    // 工厂函数：创建默认实例
    std::function<void*(void)> factory;

    // 属性列表
    std::vector<PropertyInfo> properties;

    // 按名称查找属性
    const PropertyInfo* FindProperty(StringView name) const {
        for (auto& p : properties)
            if (p.name == name) return &p;
        return nullptr;
    }
};

// --- FNV-1a 64-bit 哈希（编译期和运行期均可用） ---
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

    // 注册一个类型（由 HE_REGISTER_CLASS 宏自动调用）
    void RegisterClass(const ClassInfo* info);

    // 查询
    const ClassInfo* FindClass(StringView name) const;
    const ClassInfo* FindClassByHash(u64 hash) const;

    // 遍历所有类型
    void ForEachClass(std::function<void(const ClassInfo&)> callback) const;

private:
    std::unordered_map<u64, const ClassInfo*> m_ClassMap;  // hash → 类型描述
};

// ============================================================
// 宏驱动反射 — 替代 C++26 ^T 和 [[engine::]]
// ============================================================

// HE_CLASS — 在类声明中添加，赋予反射能力
#define HE_CLASS() \
public: \
    static const ::he::reflect::ClassInfo* StaticClass(); \
    virtual const ::he::reflect::ClassInfo* GetClass() const { return StaticClass(); } \
private:

// HE_COMPONENT — 标记一个类为引擎组件
#define HE_COMPONENT() HE_CLASS()

// HE_RESOURCE — 标记一个类为引擎资源
#define HE_RESOURCE() HE_CLASS()

// --- 注册宏（在 .cpp 中使用） ---
// 开始注册一个类
#define HE_BEGIN_REGISTER(ClassName) \
    const ::he::reflect::ClassInfo* ClassName::StaticClass() { \
        static ::he::reflect::ClassInfo s_Info; \
        static bool s_Initialized = false; \
        if (!s_Initialized) { \
            s_Info.name = #ClassName; \
            s_Info.size = sizeof(ClassName); \
            s_Info.typeHash = ::he::reflect::HashString(#ClassName); \
            s_Info.factory = []() -> void* { return new ClassName(); };

// 注册一个属性成员
#define HE_REGISTER_PROPERTY(Type, Member) \
            { \
                ::he::reflect::PropertyInfo prop; \
                prop.name = #Member; \
                prop.offset = offsetof(ClassName, Member); \
                prop.size = sizeof(Type); \
                prop.typeName = #Type; \
                prop.flags = ::he::reflect::PF_Serializable;

// --- 属性注解（紧跟在 HE_REGISTER_PROPERTY 后使用） ---
// 编辑器分类
#define HE_ATTR_CATEGORY(cat) \
                prop.attributes.emplace_back("Category", cat);

// 显示名称
#define HE_ATTR_DISPLAY_NAME(name) \
                prop.attributes.emplace_back("DisplayName", name);

// 数值范围
#define HE_ATTR_RANGE(minVal, maxVal) \
                prop.attributes.emplace_back("Range", std::to_string(minVal) + "," + std::to_string(maxVal));

// 工具提示
#define HE_ATTR_TOOLTIP(text) \
                prop.attributes.emplace_back("Tooltip", text);

// 只读
#define HE_ATTR_READ_ONLY() \
                prop.flags |= ::he::reflect::PF_ReadOnly;

// 编辑器隐藏
#define HE_ATTR_HIDDEN() \
                prop.flags |= ::he::reflect::PF_Hidden;

// 网络复制
#define HE_ATTR_REPLICATED() \
                prop.flags |= ::he::reflect::PF_Replicated;

// 资产选择器
#define HE_ATTR_ASSET_PICKER(filter) \
                prop.attributes.emplace_back("AssetPicker", filter);

// 颜色控件
#define HE_ATTR_COLOR() \
                prop.attributes.emplace_back("ColorWidget", "1");

// 弃用标记
#define HE_ATTR_DEPRECATED(reason) \
                prop.flags |= ::he::reflect::PF_Deprecated; \
                prop.attributes.emplace_back("Deprecated", reason);

// 结束属性注册，加入列表
#define HE_END_PROPERTY() \
                s_Info.properties.push_back(prop); \
            }

// 结束类注册
#define HE_END_REGISTER() \
            s_Initialized = true; \
            ::he::reflect::TypeRegistry::Instance().RegisterClass(&s_Info); \
        } \
        return &s_Info; \
    }

// ============================================================
// 辅助函数：通过反射读写属性
// ============================================================

// 获取成员指针（通过偏移量）
template<typename T, typename MemberType>
MemberType* GetMemberPtr(T* obj, const PropertyInfo& prop) {
    return reinterpret_cast<MemberType*>(
        reinterpret_cast<char*>(obj) + prop.offset);
}

// 遍历一个对象的所有反射属性
template<typename T>
void ForEachProperty(T* obj, std::function<void(const PropertyInfo&, void*)> callback) {
    const ClassInfo* cls = obj->GetClass();
    if (!cls) return;

    // 递归遍历父类属性
    if (cls->parent) {
        // 注意：需要类型擦除方式调用父类的 GetClass
    }

    for (auto& prop : cls->properties) {
        void* ptr = reinterpret_cast<char*>(obj) + prop.offset;
        callback(prop, ptr);
    }
}

} // namespace he::reflect
