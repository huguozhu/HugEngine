#pragma once

#include "Core/Types.h"
#include "Containers/Array.h"
#include <functional>
#include <unordered_map>
#include <variant>

// ============================================================
// CVar — 运行时控制台变量系统
//
// 用法:
//   CVar<int> r_width("r.width", 1920);
//   int w = r_width.Get();
//   r_width.Set(1280);
//
// Console 中: r.width 1280
// ============================================================

namespace he {

/// CVar 值类型
using CVarValue = std::variant<i32, f32, String, bool>;

/// CVar 基础类
class CVarBase {
public:
    CVarBase(StringView name, StringView desc);
    virtual ~CVarBase() = default;

    const String& GetName()        const { return m_Name; }
    const String& GetDescription() const { return m_Description; }

    virtual CVarValue GetValue()  const = 0;
    virtual String    GetString() const = 0;
    virtual void      SetFromString(StringView str) = 0;

    /// 获取所有注册的 CVar
    static const TArray<CVarBase*>& GetAll();

protected:
    String m_Name;
    String m_Description;
};

/// 类型化 CVar
template<typename T>
class CVar : public CVarBase {
public:
    CVar(StringView name, T defaultValue, StringView desc = "")
        : CVarBase(name, desc), m_Value(defaultValue) {}

    T    Get() const { return m_Value; }
    void Set(T val)  { m_Value = val; }

    CVarValue GetValue()  const override { return CVarValue(m_Value); }
    String    GetString() const override {
        if constexpr (std::is_same_v<T, i32>)      return std::to_string(m_Value);
        else if constexpr (std::is_same_v<T, f32>)  return std::to_string(m_Value);
        else if constexpr (std::is_same_v<T, bool>) return m_Value ? "true" : "false";
        else if constexpr (std::is_same_v<T, String>) return m_Value;
        return "?";
    }

    void SetFromString(StringView str) override {
        if constexpr (std::is_same_v<T, i32>)      { m_Value = static_cast<i32>(std::stoi(String(str))); }
        else if constexpr (std::is_same_v<T, f32>)  { m_Value = std::stof(String(str)); }
        else if constexpr (std::is_same_v<T, bool>) { m_Value = (str == "true" || str == "1"); }
        else if constexpr (std::is_same_v<T, String>){ m_Value = String(str); }
    }

private:
    T m_Value;
};

/// 根据名称查找 CVar
CVarBase* FindCVar(StringView name);

} // namespace he
