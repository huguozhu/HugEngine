#pragma once

#include "Scene/Component.h"
#include "Core/Types.h"

#include <vector>
#include <utility>

// ============================================================
// MemoryComponent — 智能体记忆组件
//
// MVP：短期记忆（KV 风格，超限淘汰最旧）。
// 长期/情景记忆留作后续扩展（接口占位）。
// ============================================================

namespace he::ai {

/// 单条记忆
struct MemoryEntry {
    String key;          // 记忆键
    String value;        // 记忆内容
    f32    importance = 0.5f;   // 重要度 [0,1]
};

/// 记忆组件：短期记忆容器（反射可序列化字段留后续）
class MemoryComponent : public he::Component {
    HE_COMPONENT()
public:
    /// 写入一条短期记忆（超限时淘汰最旧）
    void AddShortTerm(const String& key, const String& value, f32 importance = 0.5f);

    /// 查询记忆（命中返回 true 并写出 value）
    bool Query(const String& key, String& out) const;

    usize GetShortTermCount() const { return m_ShortTerm.size(); }
    void  ClearShortTerm() { m_ShortTerm.clear(); }

    // 长期/情景记忆：后续扩展（如按重要度滚动归档）
    // void AddLongTerm(...);

private:
    static constexpr usize kMaxShortTerm = 64;              // 短期记忆上限
    std::vector<MemoryEntry> m_ShortTerm;                   // 按插入序保存
};

} // namespace he::ai
