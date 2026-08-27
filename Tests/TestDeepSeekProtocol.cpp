// ============================================================
// Tests/TestDeepSeekProtocol.cpp — DeepSeek 请求体构造单元测试
//
// 只测不联网的纯函数 BuildRequestBody（请求体 JSON 结构）。
// ============================================================

#include "doctest.h"

#include "AI/DeepSeekClient.h"

using namespace he;
using namespace he::ai;

TEST_CASE("DeepSeekClient::BuildRequestBody 包含 model 与 json_object 模式") {
    String body = DeepSeekClient::BuildRequestBody("deepseek-chat", "你是助手", "你好");
    CHECK(body.find("deepseek-chat") != String::npos);
    CHECK(body.find("json_object") != String::npos);
    CHECK(body.find("system") != String::npos);
    CHECK(body.find("user") != String::npos);
}
