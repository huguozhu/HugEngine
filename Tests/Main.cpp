// ============================================================
// Tests/Main.cpp — doctest 测试入口
//
// 自定义 main()：先初始化 he::Logger（测试环境没有引擎初始化，
// 而 MeshComponent/SceneBuilder 等会打日志，空日志器会崩溃），
// 再运行 doctest 全部 TEST_CASE。
// ============================================================
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include "Core/Log.h"

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    // 本工程以 /utf-8 编译，测试名/日志均为 UTF-8 字节；
    // Windows 控制台默认代码页可能不是 UTF-8（如中文系统 GBK 936），
    // 按 GBK 解码 UTF-8 会显示乱码，这里把输出代码页切到 UTF-8。
    SetConsoleOutputCP(CP_UTF8);
#endif

    // 初始化 spdlog（输出到 stdout；无引擎时也必须可用）
    he::Logger::Initialize();

    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);
    int res = ctx.run();

    he::Logger::Shutdown();
    return res;
}
