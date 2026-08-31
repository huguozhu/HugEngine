#pragma once

#include "AI/SceneBuilder.h"

// ============================================================
// PromptToScene — 端到端胶水：prompt → LLM → 场景 JSON → World
//
// 1) 自动拼接 system prompt（注入 BuildTypeSchema 作为词汇表）
// 2) 调 ILLMClient（远程 DeepSeek 或测试用 FakeLLM）
// 3) 解析 OpenAI 兼容响应，取出 choices[0].message.content
// 4) 交给 BuildScene 解释成真实 Entity/Component
// ============================================================

namespace he {
class World;
class SceneGraph;
} // namespace he

namespace he::ai {
class ILLMClient;
} // namespace he::ai

namespace he::ai {

/// 拼接 system prompt（角色设定 + 输出格式 + 组件词汇表 + 规则）。
/// PromptToScene 与 GenerativeAssetFactory 共用。
String BuildSceneSystemPrompt();

/// 从 OpenAI 兼容响应中提取场景 JSON 文本（choices[0].message.content）。
/// 非法响应返回空串（AIGC 生成阶段用：只取规格，不装配实体）。
String ExtractSceneJsonFromResponse(const String& response);

/// 解析 OpenAI 兼容 LLM 响应（choices[0].message.content）并构建场景。
/// PromptToScene 与 GenerativeAssetFactory 共用；响应非法时返回失败结果。
SceneBuildResult ParseLLMResponseToScene(World& world, SceneGraph& sg,
                                         const String& response);

/// 端到端：prompt → LLM → 场景 JSON → BuildScene。
/// system prompt 自动注入组件 schema；LLM 输出经解析后交给 BuildScene。
SceneBuildResult PromptToScene(ILLMClient& llm, World& world, SceneGraph& sg,
                               const String& prompt);

} // namespace he::ai
