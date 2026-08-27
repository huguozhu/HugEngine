#pragma once

#include "Core/Types.h"

// ============================================================
// LLMClient — 大模型客户端接口
//
// 抽象「大模型对话」这一动作，便于测试时替换为假实现（FakeLLM）。
// Chat 返回 OpenAI 兼容的原始 HTTP 响应 JSON 文本，
// 其中 choices[0].message.content 是模型输出的正文。
// ============================================================

namespace he::ai {

/// LLM 客户端接口 —— 抽象「大模型对话」这一动作，便于测试替换
class ILLMClient {
public:
    virtual ~ILLMClient() = default;

    /// 发送 system + user 两条消息，返回原始 HTTP 响应 JSON 文本。
    /// 响应为 OpenAI 兼容格式：choices[0].message.content 是模型输出。
    virtual String Chat(const String& systemPrompt, const String& userPrompt) = 0;
};

} // namespace he::ai
