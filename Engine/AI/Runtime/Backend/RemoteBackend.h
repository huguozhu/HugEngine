#pragma once

#include "AI/Runtime/Backend/IAIBackend.h"
#include "AI/DeepSeekClient.h"

// ============================================================
// RemoteBackend — 远程 LLM 后端
//
// IAIBackend 的实现单元之一：承载大模型（LLM）文本生成。
// 不处理张量推理（GPU/CPU 后端在 A3 落地），
// 因此 Supports()/Load()/Allocate()/Run() 均返回不支持。
// Chat/ChatStream 是 LLM 的领域能力，由 IAIDevice 门面委托。
// ============================================================

namespace he::ai {

class RemoteBackend : public IAIBackend {
public:
    explicit RemoteBackend(String apiKey, String model = "deepseek-chat");

    // --- IAIBackend 通用推理接口：LLM 后端不处理张量，全部不支持 ---
    bool Supports(AIModelFormat fmt) const override { (void)fmt; return false; }
    Ref<IAIModel> Load(AIModelFormat fmt, Span<const u8> weights, const String& path) override {
        (void)fmt; (void)weights; (void)path; return nullptr;
    }
    Ref<IAITensor> Allocate(const AITensorDesc& desc) override { (void)desc; return nullptr; }
    Ref<IAIInference> Run(InferenceRequest&& req) override { (void)req; return nullptr; }

    // --- LLM 能力 ---
    // 一次对话，返回完整响应文本
    String Chat(const String& systemPrompt, const String& userPrompt);
    // 流式对话：MVP 先把完整响应取回，再按块切分回调 onToken（同步调用）。
    // 真实 SSE 增量流式留待后续；投递到主线程由 IAIDevice 门面负责。
    void ChatStream(const String& systemPrompt, const String& userPrompt,
                    std::function<void(const String&)> onToken);

private:
    DeepSeekClient m_Client;   // 复用 OpenAI 兼容 HTTP 客户端（WinHTTP）
};

} // namespace he::ai
