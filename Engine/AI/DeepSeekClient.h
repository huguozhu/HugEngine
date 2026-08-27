#pragma once

#include "AI/LLMClient.h"

// ============================================================
// DeepSeekClient — DeepSeek 大模型客户端
//
// OpenAI 兼容的 chat/completions 协议，经 WinHTTP 走 HTTPS。
// API Key 不硬编码，由调用方从环境变量 DEEPSEEK_API_KEY 读入。
// BuildRequestBody 为纯函数，可脱离网络单测。
// ============================================================

namespace he::ai {

/// DeepSeek 大模型客户端（OpenAI 兼容 chat/completions 协议，经 WinHTTP HTTPS）
class DeepSeekClient : public ILLMClient {
public:
    /// @param apiKey DeepSeek API Key（从环境变量 DEEPSEEK_API_KEY 读入）
    /// @param model  deepseek-chat（V3，性价比高）或 deepseek-reasoner（R1）
    explicit DeepSeekClient(String apiKey, String model = "deepseek-chat");

    String Chat(const String& systemPrompt, const String& userPrompt) override;

    /// 纯函数：构造请求体 JSON（可单测，不涉及网络）
    static String BuildRequestBody(const String& model,
                                   const String& system,
                                   const String& user);

private:
    String m_ApiKey;   // DeepSeek API Key
    String m_Model;    // 模型名（默认 deepseek-chat）
};

} // namespace he::ai
