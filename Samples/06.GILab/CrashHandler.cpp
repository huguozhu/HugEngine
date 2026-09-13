// ============================================================
// 06.GILab/CrashHandler.cpp — 崩溃处理器实现（Windows / DbgHelp）
//
// 流程（全部在崩溃线程的未处理异常过滤器内执行）：
//   1. 打印异常码、异常地址、所属模块+偏移（即使没有 PDB 也有信息）
//   2. SymInitialize 加载 PDB（搜索路径 = exe 所在目录，06.GILab.pdb 就在那里）
//   3. StackWalk64 逐帧回溯，每帧用 SymFromAddr / SymGetLineFromAddr64 解析
//      出「模块!函数 (源文件:行号)」
//   4. MiniDumpWriteDump 写出 .dmp（可用 VS 打开）
//   5. 返回 EXCEPTION_CONTINUE_SEARCH，让 WER 照常记录 APPCRASH 事件
//
// 说明：StackWalk64 / SymFromAddr 在异常过滤器里调用是 DbgHelp 的常规用法；
// 崩溃路径上不做内存分配之外的复杂操作，失败只降级为"少打印一些信息"，不再抛异常。
// ============================================================

#include "CrashHandler.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX   // 工程已在命令行定义，这里加保护避免重定义警告
#endif
#include <windows.h>
#include <dbghelp.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _MSC_VER
#pragma comment(lib, "dbghelp.lib")   // MSVC：显式链接 DbgHelp
#endif

namespace he::sample {
namespace {

// ---- 输出目标（崩溃时统一走这里：stdout + 日志文件）----
std::string g_LogPath;
HANDLE      g_LogMutex = nullptr;

/// 崩溃路径上的输出：stdout + 追加日志文件
/// 用临界区式的互斥量串行化，避免多线程同时崩溃时日志交错
void Out(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(buf, sizeof(buf) - 2, fmt, args);
    va_end(args);
    if (n <= 0) return;
    // 保证以换行结尾，便于日志阅读
    if (buf[n - 1] != '\n') {
        buf[n]     = '\n';
        buf[n + 1] = '\0';
    }

    fputs(buf, stdout);
    fflush(stdout);

    if (g_LogPath.empty()) return;
    if (g_LogMutex) WaitForSingleObject(g_LogMutex, 3000);
    FILE* f = nullptr;
    if (fopen_s(&f, g_LogPath.c_str(), "a") == 0 && f) {
        fputs(buf, f);
        fclose(f);
    }
    if (g_LogMutex) ReleaseMutex(g_LogMutex);
}

/// 异常码 → 可读名称（只列常见项，其余打印十六进制）
const char* ExceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return "EXCEPTION_ACCESS_VIOLATION（访问违例）";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "EXCEPTION_ILLEGAL_INSTRUCTION（非法指令）";
    case EXCEPTION_STACK_OVERFLOW:        return "EXCEPTION_STACK_OVERFLOW（栈溢出）";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "EXCEPTION_INT_DIVIDE_BY_ZERO（整数除零）";
    case EXCEPTION_PRIV_INSTRUCTION:      return "EXCEPTION_PRIV_INSTRUCTION（特权指令）";
    case EXCEPTION_IN_PAGE_ERROR:         return "EXCEPTION_IN_PAGE_ERROR（页错误）";
    case EXCEPTION_BREAKPOINT:            return "EXCEPTION_BREAKPOINT（断点）";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED（数组越界）";
    default:                              return "未知异常码";
    }
}

/// 取主模块（exe）所在目录，作为 PDB 搜索路径
std::string ExecutableDirectory() {
    char path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::string();
    std::string s(path, n);
    const size_t pos = s.find_last_of("\\/");
    return (pos == std::string::npos) ? std::string() : s.substr(0, pos);
}

/// 解析并打印单个栈帧
void PrintFrame(HANDLE process, unsigned int index, DWORD64 addr) {
    if (addr == 0) return;

    // 所属模块 + 模块内偏移（即使没有 PDB，也能靠它配合 .map 反查）
    std::string where = "?";
    IMAGEHLP_MODULE64 mi{};
    mi.SizeOfStruct = sizeof(mi);
    if (SymGetModuleInfo64(process, addr, &mi)) {
        where = mi.ModuleName;
    }

    // 函数名 + 源文件行号
    char symBuf[sizeof(SYMBOL_INFO) + 512] = {};
    auto* sym          = reinterpret_cast<SYMBOL_INFO*>(symBuf);
    sym->SizeOfStruct  = sizeof(SYMBOL_INFO);
    sym->MaxNameLen    = 512;

    DWORD64 disp = 0;
    if (SymFromAddr(process, addr, &disp, sym)) {
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp    = 0;
        if (SymGetLineFromAddr64(process, addr, &lineDisp, &line)) {
            Out("  #%-2u %s!%s + 0x%llX   [%s:%lu]",
                index, where.c_str(), sym->Name,
                static_cast<unsigned long long>(disp), line.FileName, line.LineNumber);
        } else {
            Out("  #%-2u %s!%s + 0x%llX",
                index, where.c_str(), sym->Name, static_cast<unsigned long long>(disp));
        }
    } else {
        Out("  #%-2u %s + 0x%llX   (无符号信息)",
            index, where.c_str(), static_cast<unsigned long long>(addr));
    }
}

/// 写 minidump（失败只提示，不阻断）
void WriteMiniDump(const char* dumpPath, EXCEPTION_POINTERS* info) {
    if (!dumpPath || !*dumpPath) return;
    HANDLE file = CreateFileA(dumpPath, GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Out("[崩溃处理器] minidump 创建失败（GetLastError=%lu），跳过", GetLastError());
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId          = GetCurrentThreadId();
    mei.ExceptionPointers = info;
    mei.ClientPointers    = FALSE;

    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                      MiniDumpWithDataSegs, info ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(file);
    Out("[崩溃处理器] minidump 已写出：%s（%s）", dumpPath, ok ? "成功" : "失败");
}

/// 未处理异常过滤器：打印完整调用栈
LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info) {
    Out("");
    Out("==================== 崩溃报告 ====================");
    if (!info || !info->ExceptionRecord) {
        Out("[崩溃处理器] 异常信息为空，无法解析");
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const EXCEPTION_RECORD* er    = info->ExceptionRecord;
    const DWORD             code  = er->ExceptionCode;
    const DWORD64           fault = reinterpret_cast<DWORD64>(er->ExceptionAddress);

    Out("异常码  : 0x%08lX  %s", code, ExceptionName(code));
    Out("异常地址: 0x%016llX", static_cast<unsigned long long>(fault));
    // 访问违例的第一个参数：0=读, 1=写；第二个参数=被访问的地址
    if (code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        Out("访问类型: %s，目标地址 0x%016llX",
            er->ExceptionInformation[0] ? "写入" : "读取",
            static_cast<unsigned long long>(er->ExceptionInformation[1]));
    }
    Out("线程 ID : %lu", GetCurrentThreadId());

    // ---- 初始化符号 ----
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    const std::string exeDir = ExecutableDirectory();
    if (!SymInitialize(process, exeDir.empty() ? nullptr : exeDir.c_str(), TRUE)) {
        Out("[崩溃处理器] SymInitialize 失败（GetLastError=%lu），将只打印模块+偏移", GetLastError());
    }

    // 崩溃指令所在位置（不依赖调用栈回溯即可拿到）
    Out("");
    Out("崩溃位置：");
    PrintFrame(process, 0, fault);

    // ---- 回溯调用栈 ----
    Out("");
    Out("调用栈（由内向外）：");
    if (info->ContextRecord) {
        CONTEXT ctx = *info->ContextRecord;
        STACKFRAME64 frame{};
        frame.AddrPC.Offset    = ctx.Rip;
        frame.AddrPC.Mode      = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Rbp;
        frame.AddrFrame.Mode   = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Rsp;
        frame.AddrStack.Mode   = AddrModeFlat;

        HANDLE thread = GetCurrentThread();
        unsigned int index = 0;
        for (; index < 64; ++index) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
                break;
            }
            if (frame.AddrPC.Offset == 0) break;
            if (index == 0) continue;   // 第 0 帧就是"崩溃位置"，上面已打印
            PrintFrame(process, index, frame.AddrPC.Offset);
        }
        if (index == 0) Out("  （StackWalk64 无法回溯，可能栈已损坏）");
    }

    // ---- minidump ----
    if (!exeDir.empty()) {
        const std::string dumpPath = exeDir + "\\06.GILab_crash.dmp";
        WriteMiniDump(dumpPath.c_str(), info);
    }

    SymCleanup(process);
    Out("==================== 崩溃报告结束 ====================");
    fflush(stdout);

    // 故意交还给系统默认处理：保留 WER 的 APPCRASH 事件与错误偏移记录
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void InstallCrashHandler(const char* logPath, const char* dumpPath) {
    (void)dumpPath;   // dump 路径由处理器内部按 exe 目录推导，参数保留供扩展
    if (logPath && *logPath) g_LogPath = logPath;
    if (!g_LogMutex) g_LogMutex = CreateMutexA(nullptr, FALSE, nullptr);

    // 覆盖运行库可能已安装的过滤器，确保我们的处理器优先生效
    SetUnhandledExceptionFilter(OnUnhandledException);

    // 安装即写一行，便于确认处理器确实生效（也顺带暴露日志文件是否可写）
    Out("[崩溃处理器] 已安装（PDB 搜索目录：%s）", ExecutableDirectory().c_str());
}

} // namespace he::sample

#else   // 非 Windows：本工程只支持 Windows，这里留空实现避免编译报错

namespace he::sample {
void InstallCrashHandler(const char*, const char*) {}
}

#endif  // _WIN32
