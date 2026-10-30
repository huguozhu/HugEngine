// ============================================================================
//  ShaderToDxil —— D3D 后端的着色器构建工具
//
//  职责：把 `slangc -target hlsl` 输出的 HLSL 文本处理成编译器能吃、且语义完整
//        的 HLSL，再编译成字节码，最后写成一个 C++ 头文件（字节数组）。
//    · 默认模式（D3D12）：调用 dxc 生成 DXIL（SM 6.6）。
//    · --dxbc 模式（D3D11）：寄存器去掉 space 并重新分配，生成 DXBC（SM 5.0），
//      并把「vk::binding → D3D11 槽位」绑定表附在 DXBC 之后（见 RHI/DxbcBindTable.h）。
//
//  为什么需要这个工具（而不是直接 slangc -target dxil）：
//    1. **uint8_t**：Slang 生成的 HLSL 里会出现 `vector<uint8_t,4>`，
//       但 HLSL/DXC 不认识 `uint8_t`（实测 dxc 1.10 报
//       "use of undeclared identifier 'uint8_t'"）⇒ 必须文本替换成 `uint`。
//    2. **语义注入**：Slang 生成的输出/输入结构体成员**没有语义**，
//       DXC 会报 VS-PS 链接错误或直接把顶点位置当成普通 varying
//       （顶点不进裁剪空间 ⇒ 画面全黑）⇒ 必须按成员名注入语义。
//    3. **[loop]**：DXC 会对循环做激进展开，某些体积雾/后处理循环展开后
//       编译时间爆炸 ⇒ 给 for 循环加 `[loop]` 属性抑制展开。
//  各补丁的实现见 ShaderPatch.h / ShaderPatch.cpp。
//
//  用法：
//    ShaderToDxil [--dxbc] <input.hlsl> <entry> <stage> <output.h> <symbol> [profile]
//    stage: vs / ps / cs / ms / as / lib（--dxbc 只支持 vs / ps / cs）
// ============================================================================

#include "ShaderPatch.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

// ---------------------------------------------------------------------------
//  文件读写
// ---------------------------------------------------------------------------
static bool ReadFile(const char* path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static bool WriteFile(const char* path, const std::string& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return true;
}

// ---------------------------------------------------------------------------
//  生成 C++ 头文件
// ---------------------------------------------------------------------------
static std::string MakeHeader(const std::vector<unsigned char>& data, const char* symbol) {
    std::string h;
    h += "// 本文件由 Tools/ShaderToDxil 自动生成，请勿手改。\n";
    h += "#pragma once\n";
    h += "static const unsigned char ";
    h += symbol;
    h += "[] = {";
    for (size_t i = 0; i < data.size(); ++i) {
        if (i % 16 == 0) h += "\n    ";
        h += std::to_string(static_cast<unsigned>(data[i]));
        h += ",";
    }
    h += "\n};\n";
    h += "static const unsigned int ";
    h += symbol;
    h += "_size = ";
    h += std::to_string(data.size());
    h += "u;\n";
    return h;
}

// ---------------------------------------------------------------------------
//  D3D11（--dxbc）：补丁 + 寄存器重新分配
// ---------------------------------------------------------------------------
static int RunDxbc(const char* input, const char* entry, const char* stage,
                   const char* output, const char* symbol) {
    using namespace shaderpatch;
    const std::string st = stage;
    if (st != "vs" && st != "ps" && st != "cs") {
        std::fprintf(stderr, "ShaderToDxil --dxbc: 不支持的阶段 %s（只支持 vs / ps / cs）\n", stage);
        return 1;
    }

    std::string src;
    if (!ReadFile(input, src)) {
        std::fprintf(stderr, "ShaderToDxil: 无法读取 %s\n", input);
        return 1;
    }

    std::string patched = InjectSemantics(src, st == "vs");
    PatchUint8(patched);
    PatchLoopsFxc(patched);
    if (st == "vs") PatchInstanceId(patched);
    D3D11RegisterResult regs = PatchRegistersD3D11(patched, st == "ps");

    const std::string hlslPath = std::string(output) + ".hlsl";
    if (!WriteFile(hlslPath.c_str(), regs.hlsl)) {
        std::fprintf(stderr, "ShaderToDxil: 无法写临时 HLSL %s\n", hlslPath.c_str());
        return 1;
    }
    if (!regs.errors.empty()) {
        std::fprintf(stderr, "ShaderToDxil --dxbc: %s 无法用于 D3D11（%zu 处寄存器问题）：\n",
                     input, regs.errors.size());
        for (const auto& e : regs.errors) std::fprintf(stderr, "    %s\n", e.c_str());
        return 1;
    }
    for (const auto& b : regs.bindings) {
        std::fprintf(stderr, "    绑定 set=%u binding=%u type=%u -> slot %u (x%u)\n",
                     b.set, b.binding, b.type, b.slot, b.count);
    }

    // TODO(M1.2)：进程内 D3DCompile 生成 DXBC，附上绑定表后写 C++ 头文件
    (void)entry; (void)symbol;
    std::fprintf(stderr, "ShaderToDxil --dxbc: 已生成补丁后的 HLSL %s（编译步骤尚未接入）\n", hlslPath.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
//  D3D12（默认）：补丁 + dxc
// ---------------------------------------------------------------------------
static int RunDxil(const char* input, const char* entry, const char* stage,
                   const char* output, const char* symbol, const char* profileArg) {
    using namespace shaderpatch;
    std::string src;
    if (!ReadFile(input, src)) {
        std::fprintf(stderr, "ShaderToDxil: 无法读取 %s\n", input);
        return 1;
    }

    const bool isVertex = (std::string(stage) == "vs" || std::string(stage) == "ms");
    std::string patched = InjectSemantics(src, isVertex);
    PatchUint8(patched);
    // 不要把所有 cbuffer 归一到 b0（曾经的 PatchCBuffers，已删除）：多个 cbuffer 会重叠
    //（dxc 报 "resource X at register 0 overlaps with resource Y"）。
    // 正确的做法是保留着色器自己的寄存器分配（b0/b1/...），根签名用 root CBV 覆盖 b0..b7。
    PatchLoops(patched);
    if (std::string(stage) == "vs") PatchInstanceId(patched);
    // 寄存器按 vk::binding 重排，与 D3D12 后端的「register = binding、space = set」约定一致
    patched = PatchRegisters(patched);

    // 落一份补丁后的 HLSL 便于排查（设 HE_DUMP_DXIL_HLSL=path 覆盖默认名）
    if (const char* dump = std::getenv("HE_DUMP_DXIL_HLSL")) {
        WriteFile(dump, patched);
    }

    const std::string hlslPath = std::string(output) + ".hlsl";
    if (!WriteFile(hlslPath.c_str(), patched)) {
        std::fprintf(stderr, "ShaderToDxil: 无法写临时 HLSL %s\n", hlslPath.c_str());
        return 1;
    }

    // 目标 profile
    char profile[32];
    if (profileArg) {
        std::snprintf(profile, sizeof(profile), "%s", profileArg);
    } else {
        std::string s = stage;
        const char* t = (s == "vs") ? "vs_6_6" : (s == "ps") ? "ps_6_6"
                      : (s == "cs") ? "cs_6_6" : (s == "ms") ? "ms_6_6"
                      : (s == "as") ? "as_6_6" : "lib_6_6";
        std::snprintf(profile, sizeof(profile), "%s", t);
    }

    // 调用 dxc
    const std::string dxilPath = std::string(output) + ".dxil";
    std::string cmd = "dxc.exe -T ";
    cmd += profile;
    cmd += " -E ";
    cmd += entry;
    cmd += " -Fo \"";
    cmd += dxilPath;
    cmd += "\" \"";
    cmd += hlslPath;
    cmd += "\"";

    std::fprintf(stderr, "ShaderToDxil: %s\n", cmd.c_str());
    if (std::system(cmd.c_str()) != 0) {
        std::fprintf(stderr, "ShaderToDxil: dxc 编译失败（%s）\n", input);
        return 1;
    }

    std::string dxil;
    if (!ReadFile(dxilPath.c_str(), dxil)) {
        std::fprintf(stderr, "ShaderToDxil: 无法读取 DXIL %s\n", dxilPath.c_str());
        return 1;
    }

    std::vector<unsigned char> bytes(dxil.begin(), dxil.end());
    if (!WriteFile(output, MakeHeader(bytes, symbol))) {
        std::fprintf(stderr, "ShaderToDxil: 无法写 %s\n", output);
        return 1;
    }
    std::fprintf(stderr, "    DXIL: %s (%zu 字节) -> %s\n", symbol, bytes.size(), output);
    return 0;
}

// ---------------------------------------------------------------------------
//  main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    // 可选的第一个参数 --dxbc 选择 D3D11 模式，其余参数位置不变
    const bool dxbc = (argc >= 2 && std::strcmp(argv[1], "--dxbc") == 0);
    if (dxbc) { --argc; ++argv; }

    if (argc < 6) {
        std::fprintf(stderr,
            "用法: ShaderToDxil [--dxbc] <input.hlsl> <entry> <stage> <output.h> <symbol> [profile]\n"
            "  stage: vs / ps / cs / ms / as / lib（--dxbc 只支持 vs / ps / cs）\n");
        return 1;
    }
    const char* input   = argv[1];
    const char* entry   = argv[2];
    const char* stage   = argv[3];
    const char* output  = argv[4];
    const char* symbol  = argv[5];
    const char* profileArg = (argc >= 7) ? argv[6] : nullptr;

    return dxbc ? RunDxbc(input, entry, stage, output, symbol)
                : RunDxil(input, entry, stage, output, symbol, profileArg);
}
