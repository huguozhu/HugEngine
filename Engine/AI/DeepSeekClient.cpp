#include "AI/DeepSeekClient.h"

#include "Core/Log.h"
#include "nlohmann/json.hpp"

#include <windows.h>
#include <winhttp.h>
#include <vector>
#include <utility>
#pragma comment(lib, "winhttp.lib")

namespace he::ai {

DeepSeekClient::DeepSeekClient(String apiKey, String model)
    : m_ApiKey(std::move(apiKey)), m_Model(std::move(model)) {}

String DeepSeekClient::BuildRequestBody(const String& model,
                                        const String& system,
                                        const String& user) {
    nlohmann::json req;
    req["model"] = model;
    req["messages"] = {
        { {"role","system"}, {"content", system} },
        { {"role","user"},   {"content", user} }
    };
    req["response_format"] = { {"type","json_object"} };  // 强制模型输出合法 JSON
    req["stream"] = false;
    req["temperature"] = 0.2;   // 低温度：场景生成更稳定
    return req.dump();
}

// UTF-8 String → 宽字符（WinHTTP 需要宽字符）
static std::wstring ToWide(const String& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

String DeepSeekClient::Chat(const String& systemPrompt, const String& userPrompt) {
    // 构造请求体（OpenAI 兼容 chat/completions）
    String body = BuildRequestBody(m_Model, systemPrompt, userPrompt);

    // 1. 打开 WinHTTP 会话
    HINTERNET hSession = WinHttpOpen(L"HugEngine/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        HE_CORE_ERROR("[DeepSeek] WinHttpOpen 失败");
        return {};
    }

    // 2. 连接 DeepSeek API 服务器（HTTPS）
    HINTERNET hConnect = WinHttpConnect(hSession, L"api.deepseek.com",
                                        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        HE_CORE_ERROR("[DeepSeek] WinHttpConnect 失败");
        WinHttpCloseHandle(hSession);
        return {};
    }

    // 3. 打开 POST 请求
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", L"/chat/completions", nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        HE_CORE_ERROR("[DeepSeek] WinHttpOpenRequest 失败");
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    // 4. 设置请求头：JSON 内容类型 + Bearer 鉴权
    std::wstring headers = L"Content-Type: application/json\r\n";
    headers += L"Authorization: Bearer " + ToWide(m_ApiKey) + L"\r\n";

    // 5. 发送请求并读取响应（循环读满所有数据块）
    BOOL ok = WinHttpSendRequest(hRequest, headers.c_str(), (DWORD)headers.size(),
                                 (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
    String response;
    if (ok) {
        ok = WinHttpReceiveResponse(hRequest, nullptr);
        DWORD bytesAvail = 0;
        do {
            bytesAvail = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &bytesAvail)) break;
            if (bytesAvail == 0) break;
            std::vector<char> buf(bytesAvail);
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, buf.data(), bytesAvail, &read)) break;
            response.append(buf.data(), read);
        } while (bytesAvail > 0);
    } else {
        HE_CORE_ERROR("[DeepSeek] WinHttpSendRequest 失败, err={}", GetLastError());
    }

    // 6. 关闭句柄（先请求、再连接、后会话）
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

} // namespace he::ai
