#include "AI/Agent/MemoryComponent.h"

namespace he::ai {

void MemoryComponent::AddShortTerm(const String& key, const String& value, f32 importance) {
    // 键已存在则覆盖（保持插入序）
    for (auto& e : m_ShortTerm) {
        if (e.key == key) {
            e.value = value;
            e.importance = importance;
            return;
        }
    }
    // 超限：淘汰最旧一条（队首）
    if (m_ShortTerm.size() >= kMaxShortTerm)
        m_ShortTerm.erase(m_ShortTerm.begin());
    m_ShortTerm.push_back({key, value, importance});
}

bool MemoryComponent::Query(const String& key, String& out) const {
    for (auto& e : m_ShortTerm) {
        if (e.key == key) {
            out = e.value;
            return true;
        }
    }
    return false;
}

} // namespace he::ai
