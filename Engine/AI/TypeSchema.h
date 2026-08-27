#pragma once

#include "Core/Types.h"

// ============================================================
// TypeSchema — 组件类型 schema 生成
//
// 把引擎"能造什么组件"的清单序列化成 JSON 字符串，
// 注入 LLM 的 system prompt，作为 LLM 的「可用词汇表」：
// LLM 只能引用这里出现的类型与字段，杜绝编造不存在的组件。
// ============================================================

namespace he::ai {

/// 返回组件类型 schema JSON 字符串（注入 LLM system prompt 作为「可用词汇表」）
String BuildTypeSchema();

} // namespace he::ai
