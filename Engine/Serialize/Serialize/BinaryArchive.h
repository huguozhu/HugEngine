#pragma once

#include "Serialize/Archive.h"
#include <vector>

// ============================================================
// BinaryArchive — 紧凑二进制序列化
// ============================================================

namespace he::serialize {

class BinaryArchive : public IArchive {
public:
    explicit BinaryArchive(ArchiveMode mode);
    ~BinaryArchive() override;

    // 获取/设置数据缓冲区
    const std::vector<u8>& GetBuffer() const { return m_Buffer; }
    void SetBuffer(const std::vector<u8>& data) { m_Buffer = data; m_Pos = 0; }
    void SetBuffer(std::vector<u8>&& data) { m_Buffer = std::move(data); m_Pos = 0; }

    void Serialize(StringView name, bool& v)       override;
    void Serialize(StringView name, i32& v)        override;
    void Serialize(StringView name, u32& v)        override;
    void Serialize(StringView name, i64& v)        override;
    void Serialize(StringView name, u64& v)        override;
    void Serialize(StringView name, f32& v)        override;
    void Serialize(StringView name, f64& v)        override;
    void Serialize(StringView name, String& v)     override;
    void BeginObject(StringView name) override;
    void EndObject() override;
    void BeginArray(StringView name, u32& count) override;
    void EndArray() override;

private:
    template<typename T> void Write(const T& val);
    template<typename T> void Read(T& val);
    void WriteBytes(const void* data, usize size);
    void ReadBytes(void* data, usize size);

    std::vector<u8> m_Buffer;
    usize m_Pos = 0;
};

template<typename T>
void BinaryArchive::Write(const T& val) {
    WriteBytes(&val, sizeof(T));
}

template<typename T>
void BinaryArchive::Read(T& val) {
    ReadBytes(&val, sizeof(T));
}

} // namespace he::serialize
