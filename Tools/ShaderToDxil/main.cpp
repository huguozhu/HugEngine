// ============================================================================
//  ShaderToDxil —— D3D12 后端的着色器构建工具
//
//  职责：把 `slangc -target hlsl` 输出的 HLSL 文本处理成 dxc 能吃、且语义完整
//        的 HLSL，再调用 dxc 生成 DXIL，最后写成一个 C++ 头文件（字节数组）。
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
//
//  用法：
//    ShaderToDxil <input.hlsl> <entry> <stage> <output.h> <symbol> [profile]
//    stage: vs / ps / cs / ms / as / lib
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <cctype>

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
//  语义注入
//
//  【关键设计】TEXCOORD 索引按「成员名 + 该名字第几次出现」分配，而不是按声明序号。
//  原因：Slang 会为 VS 和 PS 各生成一份同名结构体（如 VSOutput），但**按各自实际
//  用到的成员裁剪** ⇒ 若按声明序号编号，同一个 uv 在 VS 里是 TEXCOORD1、在 PS 里
//  是 TEXCOORD0 ⇒ DXC 报
//    "Semantic 'TEXCOORD' is defined for mismatched hardware registers"
//  ⇒ 全屏后处理全部失效（该 pass 每帧报一次错误、画面黑）。
//  按名字查表 + 同名递增 ⇒ 两侧对同一成员永远得到同一个索引，且不会撞号。
// ---------------------------------------------------------------------------
struct SemanticTable {
    // 名字关键字 → 基准索引。position 单独处理（SV_Position）。
    // 未登记的名字从 kUnknownBase 起按出现顺序分配。
    static constexpr int kUnknownBase = 16;
};

/// 判断名字里是否含某个关键字（大小写敏感，够用即可）
static bool Has(const std::string& name, const char* key) {
    return name.find(key) != std::string::npos;
}

/// 为一个成员名分配语义字符串。counters 记录「该基准名已出现过几次」，用于同名递增。
static std::string AssignSemantic(const std::string& memberName,
                                 const std::string& typeText,
                                 std::map<std::string, int>& counters,
                                 bool isVertexStage) {
    const bool isFloat4 = (typeText.rfind("float4", 0) == 0);

    // position：VS 输出必须是 SV_Position（否则顶点不进裁剪空间）；PS 输入用
    // TEXCOORD0 反而更安全（PS 的 SV_Position 是像素坐标，语义不同）。
    if (Has(memberName, "position") || Has(memberName, "Position")) {
        return (isVertexStage && isFloat4) ? " : SV_Position" : " : TEXCOORD0";
    }

    // 已登记的名字 → 基准索引
    struct Entry { const char* key; int base; };
    static const Entry kTable[] = {
        { "worldPos",     1 },
        { "worldNormal",  2 },
        { "normal",       2 },
        { "uv",           3 },
        { "texCoord",     3 },
        { "tangent",      4 },
        { "bitangent",    5 },
        { "color",        6 },
        { "objectIndex",  7 },
        { "worldDir",     8 },
        { "viewDir",      9 },
        { "depth",       10 },
    };

    for (const auto& e : kTable) {
        if (Has(memberName, e.key)) {
            const std::string k = e.key;
            const int nth = counters[k]++;          // 同名第 n 次 ⇒ 索引递增
            return std::string(" : TEXCOORD") + std::to_string(e.base + nth);
        }
    }

    // 未登记：从 kUnknownBase 起顺序分配（同一结构体内顺序稳定，两侧一致）
    const int nth = counters["__unknown"]++;
    return std::string(" : TEXCOORD") + std::to_string(SemanticTable::kUnknownBase + nth);
}

/// 是否是「需要注入语义」的结构体：名字含 Output / Input
static bool IsSignatureStruct(const std::string& name) {
    return name.find("Output") != std::string::npos ||
           name.find("Input")  != std::string::npos;
}

/// 给 HLSL 里所有 Output/Input 结构体的无语文成员注入语义
static std::string InjectSemantics(std::string src, bool isVertexStage) {
    std::string out;
    out.reserve(src.size() + 4096);

    size_t pos = 0;
    while (true) {
        // 找 "struct <Name>"
        const size_t s = src.find("struct", pos);
        if (s == std::string::npos) { out.append(src, pos, std::string::npos); break; }
        out.append(src, pos, s - pos);

        size_t b = s + 6;
        while (b < src.size() && (src[b] == ' ' || src[b] == '\t')) ++b;
        size_t e = b;
        while (e < src.size() && (isalnum(static_cast<unsigned char>(src[e])) || src[e] == '_')) ++e;
        if (e == b) { out += "struct"; pos = s + 6; continue; }

        const std::string name = src.substr(b, e - b);
        const size_t braceOpen = src.find('{', e);
        if (braceOpen == std::string::npos) { out.append(src, s, std::string::npos); break; }

        // 找配对的 '}'
        int depth = 0; size_t i = braceOpen;
        while (i < src.size()) {
            if (src[i] == '{') ++depth;
            else if (src[i] == '}') { if (--depth == 0) break; }
            ++i;
        }
        if (i >= src.size()) { out.append(src, s, std::string::npos); break; }

        out.append(src, s, braceOpen - s + 1);      // "struct X {"
        const std::string body = src.substr(braceOpen + 1, i - braceOpen - 1);

        if (!IsSignatureStruct(name)) {
            out += body;
        } else {
            // 逐行处理成员
            std::map<std::string, int> counters;
            std::string nb;
            size_t bp = 0;
            while (bp <= body.size()) {
                size_t le = body.find('\n', bp);
                if (le == std::string::npos) le = body.size();
                std::string line = body.substr(bp, le - bp);
                const std::string tr = [&] {
                    size_t a = line.find_first_not_of(" \t");
                    return a == std::string::npos ? std::string() : line.substr(a);
                }();

                const bool isMember = !tr.empty() && tr[0] != '/' && tr[0] != '#' &&
                                      tr.find('(') == std::string::npos &&
                                      tr[0] != '}';
                if (isMember) {
                    if (tr.find(':') != std::string::npos) {
                        // 已有语义：保留
                    } else {
                        const size_t semi = line.rfind(';');
                        if (semi != std::string::npos) {
                            // 取类型文本（行首到第一个空白之前）与成员名（';' 之前）
                            const size_t firstNs = line.find_first_not_of(" \t");
                            size_t typeEnd = firstNs;
                            while (typeEnd < semi && line[typeEnd] != ' ' && line[typeEnd] != '\t') ++typeEnd;
                            const std::string typeText = line.substr(firstNs, typeEnd - firstNs);

                            size_t nameEnd = semi;
                            while (nameEnd > 0 && (line[nameEnd-1] == ' ' || line[nameEnd-1] == '\t')) --nameEnd;
                            size_t nameBeg = nameEnd;
                            while (nameBeg > 0 && (isalnum(static_cast<unsigned char>(line[nameBeg-1])) ||
                                                   line[nameBeg-1] == '_')) --nameBeg;
                            const std::string memberName = line.substr(nameBeg, nameEnd - nameBeg);

                            line.insert(semi, AssignSemantic(memberName, typeText, counters, isVertexStage));
                        }
                    }
                }
                nb += line;
                if (le < body.size()) nb += '\n';
                bp = le + 1;
            }
            out += nb;
        }
        out += '}';
        pos = i + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
//  文本补丁
// ---------------------------------------------------------------------------
static void ReplaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) {
        s.replace(p, from.size(), to);
        p += to.size();
    }
}

/// uint8_t → uint：Slang 用 uint8_t 表达 8 位整数，HLSL/DXC 不认识
static void PatchUint8(std::string& s) {
    ReplaceAll(s, "vector<uint8_t,", "vector<uint,");
    ReplaceAll(s, "uint8_t", "uint");
}

/// 把着色器里所有 cbuffer 的寄存器统一到 b0。
///
/// 【为什么必须做】Slang 为同一个 push-constant 块在不同的着色器阶段生成
/// **不同的 register**：顶点着色器里是 `register(b0)`，像素着色器里可能是
/// `register(b1)`。而 D3D12 的根签名只能给「root constants」分配**一个**寄存器
/// （我们用 b0）。两者不一致时 CreateGraphicsPipelineState 返回 E_INVALIDARG
/// （实测 ForwardPBR 的 PSO 就卡在这里：PS 需要 b1 的 CBV，根签名里没有）。
static std::string PatchCBuffers(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    size_t pos = 0;
    while (pos < src.size()) {
        const size_t s = src.find("cbuffer", pos);
        if (s == std::string::npos) { out.append(src, pos, std::string::npos); break; }
        out.append(src, pos, s - pos);
        // 找 "register(bN)"
        const size_t r = src.find("register(b", s);
        if (r == std::string::npos) { out.append(src, s, std::string::npos); break; }
        const size_t numBeg = r + 10;                 // "register(b" 之后
        size_t numEnd = numBeg;
        while (numEnd < src.size() && isdigit(static_cast<unsigned char>(src[numEnd]))) ++numEnd;
        if (numEnd == numBeg) { out.append(src, s, r + 10 - s); pos = r + 10; continue; }
        out.append(src, s, numBeg - s);               // 保留到 register(b
        out += '0';                                   // 寄存器号统一为 0
        pos = numEnd;
    }
    return out;
}

/// 给 for 循环加 [loop]：抑制 DXC 的激进展开（某些循环展开后编译时间爆炸）
static void PatchLoops(std::string& s) {
    size_t p = 0;
    while ((p = s.find("for (", p)) != std::string::npos) {
        // 已经在 [loop] / [unroll] 之后 ⇒ 跳过
        const size_t lookBack = (p >= 8) ? p - 8 : 0;
        const std::string prefix = s.substr(lookBack, p - lookBack);
        if (prefix.find("[loop]") == std::string::npos &&
            prefix.find("[unroll]") == std::string::npos) {
            // 只给「函数体内」的 for 加（行首是空白）
            size_t lineStart = p;
            while (lineStart > 0 && s[lineStart-1] != '\n') --lineStart;
            const std::string indent = s.substr(lineStart, p - lineStart);
            if (indent.find_first_not_of(" \t") == std::string::npos) {
                s.insert(lineStart, indent.empty() ? "" : std::string(""));
                s.insert(p, "[loop] ");
                p += 7;
            }
        }
        p += 5;
    }
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
//  main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr,
            "用法: ShaderToDxil <input.hlsl> <entry> <stage> <output.h> <symbol> [profile]\n"
            "  stage: vs / ps / cs / ms / as / lib\n");
        return 1;
    }
    const char* input   = argv[1];
    const char* entry   = argv[2];
    const char* stage   = argv[3];
    const char* output  = argv[4];
    const char* symbol  = argv[5];
    const char* profileArg = (argc >= 7) ? argv[6] : nullptr;

    std::string src;
    if (!ReadFile(input, src)) {
        std::fprintf(stderr, "ShaderToDxil: 无法读取 %s\n", input);
        return 1;
    }

    const bool isVertex = (std::string(stage) == "vs" || std::string(stage) == "ms");
    std::string patched = InjectSemantics(src, isVertex);
    PatchUint8(patched);
    // 【已回退】PatchCBuffers 会把所有 cbuffer 归一到 b0，导致多个 cbuffer 重叠
    //（dxc 报 "resource X at register 0 overlaps with resource Y"）。
    // 正确的做法是保留着色器自己的寄存器分配（b0/b1/...），根签名用 root CBV 覆盖 b0..b7。
    PatchLoops(patched);

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
