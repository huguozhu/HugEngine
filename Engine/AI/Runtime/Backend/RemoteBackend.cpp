#include "AI/Runtime/Backend/RemoteBackend.h"

#include "Core/Log.h"

namespace he::ai {

namespace {
// 流式切分块大小（字符）。MVP：完整响应按块模拟 token 流，保持顺序
constexpr usize kStreamChunkSize = 32;
} // namespace

RemoteBackend::RemoteBackend(String apiKey, String model)
    : m_Client(std::move(apiKey), std::move(model)) {}

String RemoteBackend::Chat(const String& systemPrompt, const String& userPrompt) {
    return m_Client.Chat(systemPrompt, userPrompt);
}

void RemoteBackend::ChatStream(const String& systemPrompt, const String& userPrompt,
                               std::function<void(const String&)> onToken) {
    // 1. 取完整响应（MVP 阶段先一次性拉取，非 SSE 增量）
    String full = m_Client.Chat(systemPrompt, userPrompt);
    if (!onToken) return;

    // 2. 按固定块切分，按序回调（模拟流式 token 语义）
    usize pos = 0;
    while (pos < full.size()) {
        usize n = std::min(kStreamChunkSize, full.size() - pos);
        onToken(full.substr(pos, n));
        pos += n;
    }
}

} // namespace he::ai
