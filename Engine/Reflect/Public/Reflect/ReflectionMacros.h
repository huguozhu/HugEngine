#pragma once

#include "Reflect/ReflectionAPI.h"

// ============================================================
// ReflectionMacros.h — 宏驱动反射后端（当前实现）
//
// 提供 HE_CLASS / HE_COMPONENT / HE_BEGIN_REGISTER 等宏，
// 编译器展开后生成 StaticClass() 函数，填充 ClassInfo。
//
// 切换为 C++26 后端时：
//   1. 保留 HE_CLASS() 宏（签名不变，改用 ^T 获取元数据）
//   2. 替换 HE_BEGIN_REGISTER → consteval 函数
//   3. 本文件之外的所有代码不变
// ============================================================

namespace he::reflect {

// --- 后端标记（文档用，未来 C++26 后端改为 "consteval"）---
constexpr StringView kReflectionBackend = "macro";

} // namespace he::reflect

// ============================================================
// 类声明宏
// ============================================================

#define HE_CLASS() \
public: \
    static const ::he::reflect::ClassInfo* StaticClass(); \
    virtual const ::he::reflect::ClassInfo* GetClass() const { return StaticClass(); } \
private:

#define HE_COMPONENT() HE_CLASS()
#define HE_RESOURCE()  HE_CLASS()

// ============================================================
// 注册宏（在 .cpp 中使用）
// ============================================================

#define HE_BEGIN_REGISTER(ClassName) \
    const ::he::reflect::ClassInfo* ClassName::StaticClass() { \
        static ::he::reflect::ClassInfo s_Info; \
        static bool s_Initialized = false; \
        if (!s_Initialized) { \
            s_Info.name = #ClassName; \
            s_Info.size = sizeof(ClassName); \
            s_Info.typeHash = ::he::reflect::HashString(#ClassName); \
            s_Info.factory = []() -> void* { return new ClassName(); };

#define HE_REGISTER_PROPERTY(Type, Member) \
            { \
                ::he::reflect::PropertyInfo prop; \
                prop.name = #Member; \
                prop.offset = offsetof(ClassName, Member); \
                prop.size = sizeof(Type); \
                prop.typeName = #Type; \
                prop.flags = ::he::reflect::PF_Serializable;

// --- 属性注解 ---
#define HE_ATTR_CATEGORY(cat)       prop.attributes.emplace_back("Category", cat);
#define HE_ATTR_DISPLAY_NAME(name)  prop.attributes.emplace_back("DisplayName", name);
#define HE_ATTR_RANGE(minV, maxV)   prop.attributes.emplace_back("Range", std::to_string(minV) + "," + std::to_string(maxV));
#define HE_ATTR_TOOLTIP(text)       prop.attributes.emplace_back("Tooltip", text);
#define HE_ATTR_READ_ONLY()         prop.flags |= ::he::reflect::PF_ReadOnly;
#define HE_ATTR_HIDDEN()            prop.flags |= ::he::reflect::PF_Hidden;
#define HE_ATTR_REPLICATED()        prop.flags |= ::he::reflect::PF_Replicated;
#define HE_ATTR_ASSET_PICKER(f)     prop.attributes.emplace_back("AssetPicker", f);
#define HE_ATTR_COLOR()             prop.attributes.emplace_back("ColorWidget", "1");
#define HE_ATTR_DEPRECATED(reason)  prop.flags |= ::he::reflect::PF_Deprecated; prop.attributes.emplace_back("Deprecated", reason);

#define HE_END_PROPERTY() \
                s_Info.properties.push_back(prop); \
            }

#define HE_END_REGISTER() \
            s_Initialized = true; \
            ::he::reflect::TypeRegistry::Instance().RegisterClass(&s_Info); \
        } \
        return &s_Info; \
    }
