#pragma once

#include "Core/Types.h"

// ============================================================
// ToolUse — 工具调用原语
//
// 引擎侧工具（SpawnEntity / SetTransform / SetProperty 等）的
// 名称与参数说明清单，注入 LLM 决策 prompt（function-calling 风格）。
// 工具的实际执行 = Action → CompileAction（见 WorldModel/Action.h）。
// ============================================================

namespace he::ai {

/// 工具信息（MVP：名称 + 说明；参数 schema 后续由反射自动生成）
struct ToolInfo {
    String name;          // 工具名
    String description;   // 说明（LLM 用）
};

class ToolUse {
public:
    /// 返回可用工具清单（JSON 文本，注入 LLM 决策 prompt）
    static String BuildToolSchema();

    /// 返回工具信息列表（供编辑器/面板展示）
    static const std::vector<ToolInfo>& GetTools();
};

} // namespace he::ai
