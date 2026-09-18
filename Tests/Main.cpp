// ============================================================
// Tests/Main.cpp — doctest 测试入口
//
// 自定义 main()：先初始化 he::Logger（测试环境没有引擎初始化，
// 而 MeshComponent/SceneBuilder 等会打日志，空日志器会崩溃），
// 再运行 doctest 全部 TEST_CASE。
//
// 【崩溃处理器】（D1 / 任务 2）测试可执行文件在"只跑部分用例"时**每次都在退出阶段
// 崩溃**（0xC0000005、出错模块 unknown、错误偏移 0 —— 跳到了地址 0）。没有处理器时
// 只能从 WER 事件里读到"unknown + 偏移 0"，等于没有证据；装上处理器后崩溃报告会给出
// "函数名 + 源文件:行号" 的完整调用栈与 minidump，这类"退出阶段崩溃"才查得动。
// 日志与 dump 与 06.GILab 同一约定：都放在 Content/Config 下（该目录存在且不入库）。
// ============================================================
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include "Core/Log.h"
#include "Core/CrashHandler.h"

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    // 本工程以 /utf-8 编译，测试名/日志均为 UTF-8 字节；
    // Windows 控制台默认代码页可能不是 UTF-8（如中文系统 GBK 936），
    // 按 GBK 解码 UTF-8 会显示乱码，这里把输出代码页切到 UTF-8。
    SetConsoleOutputCP(CP_UTF8);
    he::InstallCrashHandler(HUGE_CONTENT_DIR "Config/HugEngineTests_crash.log",
                            HUGE_CONTENT_DIR "Config/HugEngineTests_crash.dmp");
#endif

    // 初始化 spdlog（输出到 stdout；无引擎时也必须可用）
    he::Logger::Initialize();

    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);
    int res = ctx.run();

    he::Logger::Shutdown();
    return res;
}
