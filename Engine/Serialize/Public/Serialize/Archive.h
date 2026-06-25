#pragma once

#include "Core/Types.h"
#include "Math/Math.h"
#include "Reflect/ReflectionAPI.h"  // 只依赖稳定 API，不依赖宏后端

#include <vector>
#include <string_view>

// ============================================================
// Archive.h — 序列化抽象层（仅依赖 ReflectionAPI）
//
// 支持 Binary（紧凑）和 JSON（人类可读）两种格式。
// 消费者通过 IArchive::Serialize(name, value) 读写数据。
// ============================================================

namespace he::serialize {

// --- 归档模式 ---
enum class ArchiveMode : u8 { Read, Write };

// --- 抽象归档接口 ---
class IArchive {
public:
    explicit IArchive(ArchiveMode mode) : m_Mode(mode) {}
    virtual ~IArchive() = default;

    ArchiveMode GetMode() const { return m_Mode; }
    bool IsWriting()    const { return m_Mode == ArchiveMode::Write; }
    bool IsReading()    const { return m_Mode == ArchiveMode::Read; }

    // --- 基础类型序列化 ---
    virtual void Serialize(StringView name, bool& v)       = 0;
    virtual void Serialize(StringView name, i32& v)        = 0;
    virtual void Serialize(StringView name, u32& v)        = 0;
    virtual void Serialize(StringView name, i64& v)        = 0;
    virtual void Serialize(StringView name, u64& v)        = 0;
    virtual void Serialize(StringView name, f32& v)        = 0;
    virtual void Serialize(StringView name, f64& v)        = 0;
    virtual void Serialize(StringView name, String& v)     = 0;
    virtual void Serialize(StringView name, float3& v);
    virtual void Serialize(StringView name, float4& v);
    virtual void Serialize(StringView name, quat& v);

    // --- 结构体 ---
    virtual void BeginObject(StringView name) = 0;
    virtual void EndObject() = 0;

    // --- 数组 ---
    virtual void BeginArray(StringView name, u32& count) = 0;
    virtual void EndArray() = 0;

protected:
    ArchiveMode m_Mode;
};

// ============================================================
// 通过反射自动序列化任意反射类型
// ============================================================

template<typename T>
void SerializeObject(IArchive& ar, T* obj) {
    if (ar.IsWriting()) ar.BeginObject("obj");
    else                ar.BeginObject("obj");

    reflect::ForEachProperty<T>(obj, [&](const reflect::PropertyInfo& prop, void* ptr) {
        if (!(prop.flags & reflect::PF_Serializable)) return;

        // 根据属性类型名分派到正确的 Serialize 重载
        StringView tn = prop.typeName;

        if      (tn == "bool")   { auto* v = static_cast<bool*>(ptr);  ar.Serialize(prop.name, *v); }
        else if (tn == "i32")    { auto* v = static_cast<i32*>(ptr);   ar.Serialize(prop.name, *v); }
        else if (tn == "u32")    { auto* v = static_cast<u32*>(ptr);   ar.Serialize(prop.name, *v); }
        else if (tn == "i64")    { auto* v = static_cast<i64*>(ptr);   ar.Serialize(prop.name, *v); }
        else if (tn == "u64")    { auto* v = static_cast<u64*>(ptr);   ar.Serialize(prop.name, *v); }
        else if (tn == "f32")    { auto* v = static_cast<f32*>(ptr);   ar.Serialize(prop.name, *v); }
        else if (tn == "f64")    { auto* v = static_cast<f64*>(ptr);   ar.Serialize(prop.name, *v); }
        else if (tn == "float")  { auto* v = static_cast<float*>(ptr); ar.Serialize(prop.name, *v); }
        else if (tn == "String") { auto* v = static_cast<String*>(ptr);ar.Serialize(prop.name, *v); }
        else if (tn == "float3") { auto* v = static_cast<float3*>(ptr);ar.Serialize(prop.name, *v); }
        else if (tn == "float4") { auto* v = static_cast<float4*>(ptr);ar.Serialize(prop.name, *v); }
        else if (tn == "quat")   { auto* v = static_cast<quat*>(ptr);  ar.Serialize(prop.name, *v); }
        else {
            // 跳过未知类型
        }
    });

    ar.EndObject();
}

} // namespace he::serialize
