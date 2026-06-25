// ============================================================
// BinaryArchive.cpp — 二进制序列化实现
// ============================================================

#include "Serialize/BinaryArchive.h"
#include "Math/Math.h"

namespace he::serialize {

BinaryArchive::BinaryArchive(ArchiveMode mode) : IArchive(mode) {}

BinaryArchive::~BinaryArchive() = default;

void BinaryArchive::WriteBytes(const void* data, usize size) {
    if (m_Pos + size > m_Buffer.size())
        m_Buffer.resize(m_Pos + size);
    std::memcpy(m_Buffer.data() + m_Pos, data, size);
    m_Pos += size;
}

void BinaryArchive::ReadBytes(void* data, usize size) {
    if (m_Pos + size > m_Buffer.size()) return;
    std::memcpy(data, m_Buffer.data() + m_Pos, size);
    m_Pos += size;
}

void BinaryArchive::Serialize(StringView /*name*/, bool& v) {
    if (IsWriting()) Write(v); else Read(v);
}

void BinaryArchive::Serialize(StringView /*name*/, i32& v) {
    if (IsWriting()) Write(v); else Read(v);
}

void BinaryArchive::Serialize(StringView /*name*/, u32& v) {
    if (IsWriting()) Write(v); else Read(v);
}

void BinaryArchive::Serialize(StringView /*name*/, i64& v) {
    if (IsWriting()) Write(v); else Read(v);
}

void BinaryArchive::Serialize(StringView /*name*/, u64& v) {
    if (IsWriting()) Write(v); else Read(v);
}

void BinaryArchive::Serialize(StringView /*name*/, f32& v) {
    if (IsWriting()) Write(v); else Read(v);
}

void BinaryArchive::Serialize(StringView /*name*/, f64& v) {
    if (IsWriting()) Write(v); else Read(v);
}

void BinaryArchive::Serialize(StringView /*name*/, String& v) {
    if (IsWriting()) {
        u32 len = static_cast<u32>(v.size());
        Write(len);
        WriteBytes(v.data(), len);
    } else {
        u32 len = 0;
        Read(len);
        v.resize(len);
        ReadBytes(v.data(), len);
    }
}

void BinaryArchive::BeginObject(StringView /*name*/) {}
void BinaryArchive::EndObject() {}

void BinaryArchive::BeginArray(StringView /*name*/, u32& count) {
    if (IsWriting()) Write(count);
    else Read(count);
}

void BinaryArchive::EndArray() {}

} // namespace he::serialize

// ============================================================
// IArchive 默认实现（float3 / float4 / quat）
// ============================================================

namespace he::serialize {

void IArchive::Serialize(StringView name, float3& v) {
    Serialize(name, v.x);
    Serialize(name, v.y);
    Serialize(name, v.z);
}

void IArchive::Serialize(StringView name, float4& v) {
    Serialize(name, v.x);
    Serialize(name, v.y);
    Serialize(name, v.z);
    Serialize(name, v.w);
}

void IArchive::Serialize(StringView name, quat& v) {
    Serialize(name, v.x);
    Serialize(name, v.y);
    Serialize(name, v.z);
    Serialize(name, v.w);
}

} // namespace he::serialize
