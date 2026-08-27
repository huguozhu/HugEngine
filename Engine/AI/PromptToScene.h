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

/// 端到端：prompt → LLM → 场景 JSON → BuildScene。
/// system prompt 自动注入组件 schema；LLM 输出经解析后交给 BuildScene。
SceneBuildResult PromptToScene(ILLMClient& llm, World& world, SceneGraph& sg,
                               const String& prompt);

} // namespace he::ai
