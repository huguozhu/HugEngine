// ============================================================
// Core/CrashHandler.cpp — 崩溃处理器实现（Windows / DbgHelp）
//
// 流程（在崩溃线程上执行，**分阶段且有日志**）：
//   阶段 1  异常信息（异常码 / 地址 / 访问违例的目标地址）—— 只用 Win32 WriteFile 落盘
//   阶段 2  minidump（唯一不可再生的证据，先落盘）
//   阶段 3  符号化调用栈（DbgHelp：SymInitialize + StackWalk64 + SymFromAddr/行号）
//   阶段 4  收尾；默认把控制权交回系统（保留 WER 的 APPCRASH 事件）
//
// 【为什么整条路径不用 CRT stdio、不用互斥量、逐阶段写日志】（D1 / 任务 2 的实测教训）
//   旧实现在处理器里用 `fputs(stdout)` + `fopen/fputs/fclose` + 一个 `WaitForSingleObject`
//   互斥量。自检（HE_CRASH_TEST=1）实测指纹：**进程静默挂住、崩溃报告一行都没写、
//   没有 minidump**，于是"处理器到底跑到哪一步"完全不可知 —— 证据链本身失效，
//   这正是 D1（偶发崩溃）根因一直拿不到的**结构性原因**。
//   现在：
//     · 日志文件在安装时就打开并持有 HANDLE，崩溃路径只调 `WriteFile`（不碰 CRT 文件锁）；
//     · 每阶段开始/结束都写一行 ⇒ 万一仍然挂住，日志直接指出挂在哪一阶段；
//     · 看门狗线程（`HE_CRASH_WATCHDOG_SEC`，默认 60 秒）超时强制结束进程，
//       把"挂死"变成**有日志、有退出码、有界**的失败；
//     · 同一进程内第二次崩溃只写一行（InterlockedCompareExchange），输出不交错。
//
// 【为什么还要装向量化异常处理器（VEH）】同样来自自检实测：只装未处理过滤器时，
//   主动写空指针后进程静默挂住、报告与 dump 都没有 ⇒ 那次异常根本没走到顶层过滤器
//   （未处理过滤器可被任何后来者替换，系统组件也可能在它之前介入）。VEH（First=1）在
//   SEH 之前、对每一次异常都会被调用，于是"能不能拿到报告"不再取决于谁抢走了过滤器。
//   处理器只记录、一律返回 EXCEPTION_CONTINUE_SEARCH，让系统按原样继续处理。
// ============================================================

#include "Core/CrashHandler.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX   // 工程已在命令行定义，这里加保护避免重定义警告
#endif
#include <windows.h>
#include <tlhelp32.h>   // 线程快照（崩溃时的线程清单）
#include <dbghelp.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _MSC_VER
#pragma comment(lib, "dbghelp.lib")   // MSVC：显式链接 DbgHelp
#endif

namespace he {
namespace {

// ---- 输出目标（崩溃时统一走这里：日志文件 HANDLE + stdout HANDLE，都不经 CRT）----
HANDLE g_LogFile    = INVALID_HANDLE_VALUE;   // 安装时打开，崩溃路径只 WriteFile
HANDLE g_Stdout     = INVALID_HANDLE_VALUE;   // 同样只 WriteFile
volatile LONG g_CrashActive = 0;              // 只让第一个崩溃线程写完整报告
char   g_DumpPath[MAX_PATH] = {};             // minidump 输出路径（安装时定下）

/// 崩溃路径上的输出：格式化成栈上缓冲，再 WriteFile 到日志与 stdout。
/// **刻意不用 printf/fputs/fopen**：它们会取 CRT 的 stdio 锁，而崩溃时无从知道
/// 哪个线程正持有它（实测旧实现就是在这里永久阻塞的）。
void RawOut(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(buf, sizeof(buf) - 2, fmt, args);
    va_end(args);
    if (n <= 0) return;
    size_t len = (n < (int)sizeof(buf) - 2) ? (size_t)n : sizeof(buf) - 3;
    buf[len++] = '\n';
    buf[len]   = '\0';

    DWORD written = 0;
    if (g_LogFile != INVALID_HANDLE_VALUE) WriteFile(g_LogFile, buf, (DWORD)len, &written, nullptr);
    if (g_Stdout != INVALID_HANDLE_VALUE)  WriteFile(g_Stdout,  buf, (DWORD)len, &written, nullptr);
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
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED（数组越界）";
    default:                              return "未知异常码";
    }
}

/// 崩溃类异常码（只有这些才值得在 VEH 里写一份报告；软件断点/单步等按原样放行）
bool IsFatalFault(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return true;
    default:
        return false;
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
    alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 512] = {};
    auto* sym          = reinterpret_cast<SYMBOL_INFO*>(symBuf);
    sym->SizeOfStruct  = sizeof(SYMBOL_INFO);
    sym->MaxNameLen    = 512;

    DWORD64 disp = 0;
    if (SymFromAddr(process, addr, &disp, sym)) {
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp    = 0;
        if (SymGetLineFromAddr64(process, addr, &lineDisp, &line)) {
            RawOut("  #%-2u %s!%s + 0x%llX   [%s:%lu]",
                   index, where.c_str(), sym->Name,
                   static_cast<unsigned long long>(disp), line.FileName, line.LineNumber);
        } else {
            RawOut("  #%-2u %s!%s + 0x%llX",
                   index, where.c_str(), sym->Name, static_cast<unsigned long long>(disp));
        }
    } else {
        RawOut("  #%-2u %s + 0x%llX   (无符号信息)",
               index, where.c_str(), static_cast<unsigned long long>(addr));
    }
}

/// 写 minidump（失败只提示，不阻断）
void WriteMiniDump(const char* dumpPath, EXCEPTION_POINTERS* info) {
    if (!dumpPath || !*dumpPath) { RawOut("  （未指定 minidump 路径，跳过）"); return; }
    HANDLE file = CreateFileA(dumpPath, GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        RawOut("  minidump 创建失败（GetLastError=%lu），跳过", GetLastError());
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId          = GetCurrentThreadId();
    mei.ExceptionPointers = info;
    mei.ClientPointers    = FALSE;

    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                      MiniDumpWithDataSegs, info ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(file);
    RawOut("  minidump 已写出：%s（%s）", dumpPath, ok ? "成功" : "失败");
}

/// 看门狗：崩溃路径若卡住，超时后强制结束进程（有界失败 > 静默挂死）
DWORD WINAPI WatchdogThread(LPVOID param) {
    const DWORD seconds = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(param));
    Sleep(seconds * 1000u);
    RawOut("[看门狗] 崩溃处理超过 %lu 秒仍未收尾，强制结束进程（退出码 3）", seconds);
    TerminateProcess(GetCurrentProcess(), 3);
    return 0;
}

/// 写一份完整崩溃报告（阶段 1..4）。返回值供未处理过滤器透传。
LONG WriteReport(EXCEPTION_POINTERS* info) {
    RawOut("");
    RawOut("==================== 崩溃报告 ====================");
    RawOut("[crash phase 1/4] 异常信息");
    if (!info || !info->ExceptionRecord) {
        RawOut("  异常信息为空，无法解析");
        RawOut("==================== 崩溃报告结束 ====================");
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const EXCEPTION_RECORD* er    = info->ExceptionRecord;
    const DWORD             code  = er->ExceptionCode;
    const DWORD64           fault = reinterpret_cast<DWORD64>(er->ExceptionAddress);

    RawOut("  异常码  : 0x%08lX  %s", code, ExceptionName(code));
    RawOut("  异常地址: 0x%016llX", static_cast<unsigned long long>(fault));
    // 访问违例的第一个参数：0=读, 1=写；第二个参数=被访问的地址
    if (code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        RawOut("  访问类型: %s，目标地址 0x%016llX",
               er->ExceptionInformation[0] ? "写入" : "读取",
               static_cast<unsigned long long>(er->ExceptionInformation[1]));
    }
    RawOut("  线程 ID : %lu", GetCurrentThreadId());

    // ---- 看门狗 ----
    DWORD watchdogSec = 60;
    if (const char* env = std::getenv("HE_CRASH_WATCHDOG_SEC")) {
        const int v = std::atoi(env);
        if (v > 0) watchdogSec = static_cast<DWORD>(v);
    }
    if (HANDLE wd = CreateThread(nullptr, 0, WatchdogThread,
                                 reinterpret_cast<LPVOID>(static_cast<ULONG_PTR>(watchdogSec)), 0, nullptr)) {
        RawOut("  （看门狗 %lu 秒；可用 HE_CRASH_WATCHDOG_SEC 调整）", watchdogSec);
        CloseHandle(wd);   // 线程继续跑；句柄不再需要
    }

    // ---- 阶段 2：minidump 先落盘 ----
    // 【顺序很重要】符号解析（DbgHelp 加载 PDB + StackWalk64）可能很慢甚至卡住，而它只是
    // "锦上添花"；minidump 才是唯一不可再生的证据。先写 dump，保证即使符号化失败或超时，
    // 现场仍然留着（可离线用调试器解析）。
    RawOut("[crash phase 2/4] minidump");
    WriteMiniDump(g_DumpPath, info);

    // ---- 阶段 3：符号化 ----
    RawOut("[crash phase 3/4] 符号化调用栈");
    const std::string exeDir = ExecutableDirectory();
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (!SymInitialize(process, exeDir.empty() ? nullptr : exeDir.c_str(), TRUE)) {
        RawOut("  SymInitialize 失败（GetLastError=%lu），将只打印模块+偏移", GetLastError());
    }

    // 崩溃指令所在位置（不依赖调用栈回溯即可拿到）
    RawOut("  崩溃位置：");
    PrintFrame(process, 0, fault);

    RawOut("");
    RawOut("  调用栈（由内向外）：");
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
        if (index == 0) {
            RawOut("  （StackWalk64 无法回溯：栈/帧指针已损坏。改用栈扫描找候选返回地址）");
            // 兜底：帧指针坏了时 StackWalk64 一步都走不动。直接在 [Rsp, Rsp+32KB) 里扫
            // "落在本可执行文件里" 的 qword 当候选返回地址 —— 会掺入少量巧合匹配，
            // 但足以指出**是谁调用了这里**，而那正是崩溃报告最要紧的一句话。
            const auto* lo = reinterpret_cast<const DWORD64*>(ctx.Rsp);
            size_t scanned = 4096;   // 32 KB
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(lo, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) {
                const size_t avail =
                    (reinterpret_cast<size_t>(mbi.BaseAddress) + mbi.RegionSize -
                     reinterpret_cast<size_t>(lo)) / sizeof(DWORD64);
                if (avail < scanned) scanned = avail;
            }
            unsigned int hits = 0;
            for (size_t i = 0; i < scanned && hits < 24; ++i) {
                const DWORD64 candidate = lo[i];
                if (candidate < 0x10000) continue;
                const DWORD64 base = SymGetModuleBase64(process, candidate);
                if (base == 0) continue;
                char modName[MAX_PATH] = {};
                if (!GetModuleFileNameA(reinterpret_cast<HMODULE>(base), modName, MAX_PATH)) continue;
                // 只看本可执行文件里的候选：栈上到处是模块地址，全打出来会淹掉信号
                if (!strstr(modName, ".exe")) continue;
                RawOut("    [栈扫描 +0x%04llX] 候选：",
                       static_cast<unsigned long long>(i * sizeof(DWORD64)));
                PrintFrame(process, 100u + hits, candidate);
                hits++;
            }
            if (hits == 0) RawOut("    （栈扫描没有找到本模块的候选返回地址）");
        }
    } else {
        RawOut("  （没有上下文记录，无法回溯）");
    }

    // ---- 崩溃时的线程清单 ----
    // "退出阶段崩溃"往往是**别的线程**还在跑（或静态析构顺序问题），所以线程清单 +
    // 每个线程的启动地址是必要证据：启动地址能直接说出是哪个模块的哪个函数起的线程。
    RawOut("");
    RawOut("  崩溃时的线程（启动地址已符号化）：");
    {
        const DWORD pid  = GetCurrentProcessId();
        const DWORD self = GetCurrentThreadId();
        if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)) {
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            unsigned int n = 0;
            if (Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID != pid) continue;
                    n++;
                    if (n > 64) break;
                    DWORD64 start = 0;
                    if (HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID)) {
                        // 线程启动地址：NtQueryInformationThread(ThreadQuerySetWin32StartAddress = 9)
                        using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
                        if (HMODULE nt = GetModuleHandleA("ntdll.dll")) {
                            auto fn = reinterpret_cast<NtQueryInformationThreadFn>(
                                GetProcAddress(nt, "NtQueryInformationThread"));
                            ULONG ret = 0;
                            if (fn) fn(th, 9, &start, sizeof(start), &ret);
                        }
                        CloseHandle(th);
                    }
                    RawOut("    线程 %-6lu%s 启动地址 0x%016llX",
                           te.th32ThreadID, (te.th32ThreadID == self) ? "（崩溃线程）" : "          ",
                           static_cast<unsigned long long>(start));
                    if (start) PrintFrame(process, 200u + n, start);
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
            RawOut("    共 %u 个线程", n);
        } else {
            RawOut("    （无法枚举线程）");
        }
    }

    SymCleanup(process);
    RawOut("[crash phase 4/4] 崩溃报告结束");
    RawOut("==================== 崩溃报告结束 ====================");

    // `HE_CRASH_NO_WER=1` 时就地结束进程（自动化 harness 用：WER 收集转储很慢，
    // 会让"崩溃一次"的用例变成"挂住几十秒"）。
    if (std::getenv("HE_CRASH_NO_WER")) {
        RawOut("（HE_CRASH_NO_WER=1：跳过 WER，直接以退出码 2 结束）");
        TerminateProcess(GetCurrentProcess(), 2);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/// 向量化异常处理器（First=1 ⇒ 最先被调用）
LONG WINAPI OnVectoredException(PEXCEPTION_POINTERS info) {
    if (!info || !info->ExceptionRecord || !IsFatalFault(info->ExceptionRecord->ExceptionCode)) {
        return EXCEPTION_CONTINUE_SEARCH;   // 非崩溃类异常一律放行，不改语义
    }
    // 只让第一个崩溃线程写完整报告；后续崩溃只记一行（避免两份报告交错）
    if (InterlockedCompareExchange(&g_CrashActive, 1, 0) != 0) {
        RawOut("[崩溃处理器] 又收到一次崩溃异常（线程 %lu），只记录这一行",
               GetCurrentThreadId());
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return WriteReport(info);
}

/// 未处理异常过滤器（VEH 已报过就不重复；它仍保留，作为 VEH 被关掉时的兜底）
LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info) {
    if (g_CrashActive != 0) return EXCEPTION_CONTINUE_SEARCH;
    if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedCompareExchange(&g_CrashActive, 1, 0) != 0) return EXCEPTION_CONTINUE_SEARCH;
    return WriteReport(info);
}

} // namespace

void InstallCrashHandler(const char* logPath, const char* dumpPath) {
    // 日志文件：安装时就打开并持有句柄，崩溃路径只 WriteFile（见文件头说明）
    if (logPath && *logPath) {
        g_LogFile = CreateFileA(logPath, FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    g_Stdout = GetStdHandle(STD_OUTPUT_HANDLE);

    // minidump 路径：显式给出就用它，否则放在 exe 同目录（与旧行为一致）
    g_DumpPath[0] = '\0';
    if (dumpPath && *dumpPath) {
        strncpy_s(g_DumpPath, sizeof(g_DumpPath), dumpPath, _TRUNCATE);
    } else {
        const std::string exeDir = ExecutableDirectory();
        if (!exeDir.empty()) {
            snprintf(g_DumpPath, sizeof(g_DumpPath), "%s\\06.GILab_crash.dmp", exeDir.c_str());
        }
    }

    // 顶层未处理过滤器：先记下"先前是谁"，便于日志里看出有没有别人装过（D1 的实测教训）
    const auto prevFilter = reinterpret_cast<void*>(SetUnhandledExceptionFilter(OnUnhandledException));

    // 向量化异常处理器：First=1 ⇒ 在所有其他处理器之前被调用（见文件头说明）
    // `HE_CRASH_VEH=0` 可关掉（万一某处真的需要"VEH 语义"不受干扰）
    bool vehOn = true;
    if (const char* env = std::getenv("HE_CRASH_VEH")) vehOn = (std::atoi(env) != 0);
    if (vehOn) AddVectoredExceptionHandler(1, OnVectoredException);

    // 安装即写一行，便于确认处理器确实生效（也顺带暴露日志文件是否可写）
    RawOut("[崩溃处理器] 已安装（PDB 搜索目录：%s）", ExecutableDirectory().c_str());
    RawOut("[崩溃处理器] minidump：%s", g_DumpPath[0] ? g_DumpPath : "(未设置)");
    RawOut("[崩溃处理器] VEH=%s；顶层过滤器先前值=%s",
           vehOn ? "开" : "关",
           prevFilter ? "非空（有别人装过，已被本处理器覆盖）" : "空");
}

} // namespace he

#else   // 非 Windows：本工程只支持 Windows，这里留空实现避免编译报错

namespace he {
void InstallCrashHandler(const char*, const char*) {}
}

#endif  // _WIN32
