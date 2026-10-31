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
//    profile 缺省：vs→vs_6_8（T6：SV_StartInstanceLocation 需要 6.8），
//                  其余 ps/cs/ms/as→6_6，lib→lib_6_6
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

    // position：VS 输出必须是 SV_Position（否则顶点不进裁剪空间）；
    // PS 输入的 position 则是像素坐标，用 TEXCOORD（本项目不用）。
    if (Has(memberName, "position") || Has(memberName, "Position")) {
        if (isVertexStage && isFloat4) return " : SV_Position";
        // 非 float4 的 position 不能当 SV_Position（类型必须是 float4），
        // 退化成普通 TEXCOORD —— 但**必须占用一个序号**，
        // 否则后面的成员又从 0 开始，dxc 会报 "Semantic 'TEXCOORD' overlap at 0"。
        const int nth = counters["__seq"]++;
        return std::string(" : TEXCOORD") + std::to_string(nth);
    }

    // 【按声明顺序从 0 密集编号】
    //
    // D3D12 的 VS→PS 链接校验是按「语义 + 序号」逐项对照的，而且两侧的
    // **硬件寄存器是按声明顺序依次分配**的。因此：
    //   · 序号必须是 0..N-1 的密集序列（稀疏会出现
    //     "Semantic 'TEXCOORD' is defined for mismatched hardware registers"）
    //   · 两侧必须用**相同的声明顺序**才能对上
    //
    // 之前按「成员名查表」给固定序号（uv 恒为 TEXCOORD3），看起来两侧一致，
    // 但 PS 的输入签名里只有 uv 一个元素 ⇒ 它落在寄存器 0，而 VS 的
    // TEXCOORD3 落在寄存器 3 ⇒ 仍然判定不匹配（实测 ToneMap 卡在这里）。
    const int nth = counters["__seq"]++;
    return std::string(" : TEXCOORD") + std::to_string(nth);
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

        // 【PS 输入必须带 SV_Position】
        //
        // D3D12 的 VS→PS 链接校验按「语义 + 序号」逐项对照，而且两阶段的
        // 通用寄存器是**按签名里的出现顺序**依次分配的：
        //   VS 输出：[SV_Position, TEXCOORD0]  ⇒ TEXCOORD0 落在通用寄存器 1
        //   PS 输入：[TEXCOORD0]               ⇒ 落在通用寄存器 0
        // ⇒ 报 "Semantic 'TEXCOORD' is defined for mismatched hardware registers"。
        // 实测给 PS 输入补一个 SV_Position 成员后 ToneMap 的 PSO 立刻创建成功。
        // 这里统一给「像素阶段的输入结构」补上，保证两侧寄存器对齐。
        if (!isVertexStage && name.find("Input") != std::string::npos) {
            bool hasPos = false;
            for (const char* key : { "position", "Position" }) {
                if (body.find(key) != std::string::npos) { hasPos = true; break; }
            }
            if (!hasPos) {
                out += "\n    float4 position_0 : SV_Position;   // 对齐 VS 输出的通用寄存器编号";
            }
        }

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
//  顶点着色器的实例号：SM 6.8 起用 SV_StartInstanceLocation，取代逐实例属性绕行
//
//  【为什么必须处理】Vulkan 的 gl_InstanceIndex 含 draw 的 firstInstance（GPU 剔除把对象号、
//  Nanite 把簇号放在 firstInstance 里），而 D3D12 的 SV_InstanceID 恒从 0 起。
//  SM 6.8 之前只能绕行：把 `SV_InstanceID` 改名成逐实例属性 `HE_INSTANCEINDEX`，由后端在
//  14 号输入槽绑一张 `0,1,2,…` 的序号缓冲（IA 取逐实例数据时会自动加上起始实例）。
//  那要多一张 1M 项的缓冲、每次绘制多绑一个顶点缓冲，PSO 反射里还要特判语义。
//
//  SM 6.8 的 `SV_StartInstanceLocation` 直接给出起始实例号，于是改成：
//      instanceID = SV_InstanceID + SV_StartInstanceLocation
//  与 Vulkan 语义一致，绕行相关的东西全部不需要。
//
//  做法：给入口函数追加一个 `uint heStartInstance_ : SV_StartInstanceLocation` 参数，
//  并在函数体第一行把起始实例号加到原实例号参数上（函数体内部的所有读取都不用改）。
// ---------------------------------------------------------------------------
static void PatchStartInstanceLocation(std::string& s) {
    static const std::string kSv = ": SV_InstanceID";
    const size_t p = s.find(kSv);
    if (p == std::string::npos) return;

    // 取出语义前面的参数名（Slang 生成的是 instanceID_0 / iid_0 这类标识符）
    size_t nameEnd = p;
    while (nameEnd > 0 && (s[nameEnd - 1] == ' ' || s[nameEnd - 1] == '\t')) --nameEnd;
    size_t nameBeg = nameEnd;
    while (nameBeg > 0 && (std::isalnum(static_cast<unsigned char>(s[nameBeg - 1])) ||
                           s[nameBeg - 1] == '_')) --nameBeg;
    const std::string name = s.substr(nameBeg, nameEnd - nameBeg);
    if (name.empty()) return;

    // 1) 追加 SV_StartInstanceLocation 参数
    s.insert(p + kSv.size(), ", uint heStartInstance_ : SV_StartInstanceLocation");

    // 2) 在函数体开头把起始实例号加进实例号（参数表结束的 ')' 之后的第一个 '{'）
    const size_t rp = s.find(')', p);
    if (rp == std::string::npos) return;
    const size_t lb = s.find('{', rp);
    if (lb == std::string::npos) return;
    s.insert(lb + 1, "\n    " + name + " += heStartInstance_;");
}

/// 旧绕行（SM 6.8 以下保留）：顶点着色器的实例号改为读逐实例顶点属性 HE_INSTANCEINDEX。
/// 后端在 kInstanceIndexSlot 绑 0,1,2,… 的逐实例序号缓冲，IA 读出时自动加上起始实例。
static void PatchInstanceId(std::string& s) {
    ReplaceAll(s, ": SV_InstanceID", ": HE_INSTANCEINDEX");
}

/// 目标 profile 是否是 6.8 或更高（形如 vs_6_8 / vs_6_9 / vs_7_0）
static bool ProfileAtLeast68(const char* profile) {
    if (!profile) return false;
    const char* us = std::strchr(profile, '_');
    if (!us) return false;
    int major = 0, minor = 0;
    if (std::sscanf(us + 1, "%d_%d", &major, &minor) != 2) return false;
    return major > 6 || (major == 6 && minor >= 8);
}

// ---------------------------------------------------------------------------
//  寄存器重映射：按源码的 [[vk::binding(B, S)]] 生成 register(<t|u|s>B, spaceS)
//
//  【为什么必须做】`slangc -target hlsl` 会忽略 vk::binding，按声明顺序自动分配
//  寄存器（如 PBR.frag 里 u_Lights=t0、u_Objects=t1、u_BRDF_LUT=t13）。而 D3D12
//  后端把描述符写在「set 基址 + binding」、按 set 号绑定 space 表，即约定
//  register = binding、space = set。两者不一致时着色器会读到别的资源
//  （光源读成物体数据、BRDF LUT 读成预滤波立方体图……）⇒ 画面错误甚至设备挂起。
//  源码里已显式写了 register(...) 的声明（如 bindless 的 space2 数组）保持不动。
// ---------------------------------------------------------------------------

/// 去掉首尾空白
static std::string Trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/// 读取源码文件并按行切分（带缓存，同一 include 文件只读一次）
static const std::vector<std::string>& GetSourceLines(const std::string& path) {
    static std::map<std::string, std::vector<std::string>> s_cache;
    auto it = s_cache.find(path);
    if (it != s_cache.end()) return it->second;
    std::vector<std::string> lines;
    std::string text;
    if (ReadFile(path.c_str(), text)) {
        std::istringstream ss(text);
        std::string l;
        while (std::getline(ss, l)) lines.push_back(l);
    }
    return s_cache.emplace(path, std::move(lines)).first->second;
}

/// 从一段源码里收集整数常量：`GPU_CONST(GPU_UINT NAME = N)`、`static const uint NAME = N;`、`#define NAME N`
static void CollectConstants(const std::vector<std::string>& lines, std::map<std::string, int>& table) {
    for (const auto& raw : lines) {
        std::string l = raw;
        const size_t cm = l.find("//");
        if (cm != std::string::npos) l.resize(cm);
        size_t eq = l.find('=');
        std::string name, value;
        if (eq != std::string::npos) {
            // NAME 是 '=' 左侧最后一个标识符
            size_t ne = eq;
            while (ne > 0 && isspace(static_cast<unsigned char>(l[ne - 1]))) --ne;
            size_t nb = ne;
            while (nb > 0 && (isalnum(static_cast<unsigned char>(l[nb - 1])) || l[nb - 1] == '_')) --nb;
            name = l.substr(nb, ne - nb);
            value = l.substr(eq + 1);
        } else if (l.rfind("#define", 0) == 0) {
            std::istringstream ss(l.substr(7));
            ss >> name >> value;
        }
        value = Trim(value);
        if (name.empty() || value.empty() || !isdigit(static_cast<unsigned char>(value[0]))) continue;
        table[name] = std::atoi(value.c_str());
    }
}

/// 常量表：ShaderTypes.slang（从源码目录逐级向上查找）+ 源码文件本身
static bool ResolveInt(const std::string& token, const std::string& srcPath, int& out) {
    const std::string t = Trim(token);
    if (t.empty()) return false;
    if (isdigit(static_cast<unsigned char>(t[0]))) { out = std::atoi(t.c_str()); return true; }

    static std::map<std::string, std::map<std::string, int>> s_tables;
    auto it = s_tables.find(srcPath);
    if (it == s_tables.end()) {
        std::map<std::string, int> table;
        std::string dir = srcPath;
        for (int depth = 0; depth < 4; ++depth) {
            const size_t slash = dir.find_last_of("/\\");
            if (slash == std::string::npos) break;
            dir.resize(slash);
            const auto& types = GetSourceLines(dir + "/ShaderTypes.slang");
            if (!types.empty()) { CollectConstants(types, table); break; }
        }
        CollectConstants(GetSourceLines(srcPath), table);
        it = s_tables.emplace(srcPath, std::move(table)).first;
    }
    auto c = it->second.find(t);
    if (c == it->second.end()) return false;
    out = c->second;
    return true;
}

static std::string PatchRegisters(const std::string& src) {
    std::istringstream in(src);
    std::string out, line;
    std::string curFile;
    int nextLine = 0;   // 下一行 HLSL 对应的源码行号（0 = 未知）
    while (std::getline(in, line)) {
        if (line.rfind("#line ", 0) == 0) {
            // 形如 `#line 38 "path"` 或 `#line 76`（沿用上一个文件）
            std::istringstream ls(line.substr(6));
            ls >> nextLine;
            const size_t q1 = line.find('"');
            const size_t q2 = (q1 == std::string::npos) ? q1 : line.find('"', q1 + 1);
            if (q2 != std::string::npos) curFile = line.substr(q1 + 1, q2 - q1 - 1);
            out += line; out += '\n';
            continue;
        }
        const int srcLine = nextLine;
        if (nextLine > 0) ++nextLine;

        const size_t reg = line.find(": register(");
        const bool isCBuffer = line.find("cbuffer") != std::string::npos;
        // 无界数组（`x[]`）保持 Slang 的自动分配：D3D12 不允许同一 space 内有多个无界数组，
        // Slang 会把它们各自放进独立的 space，强行按 binding 重排会编译失败
        const bool isUnbounded = (reg != std::string::npos) &&
                                 line.substr(0, reg).find("[]") != std::string::npos;
        if (reg != std::string::npos && !isUnbounded && srcLine > 0 && !curFile.empty()) {
            const size_t clsPos = reg + 11;
            const char cls = (clsPos < line.size()) ? line[clsPos] : 0;
            const size_t close = line.find(')', clsPos);
            // 变量名：register 前最后一个标识符（去掉 Slang 加的 `_0` 后缀和数组维度）
            std::string decl = line.substr(0, reg);
            const size_t br = decl.find('[');
            if (br != std::string::npos) decl.resize(br);
            decl = Trim(decl);
            size_t nb = decl.size();
            while (nb > 0 && (isalnum(static_cast<unsigned char>(decl[nb - 1])) || decl[nb - 1] == '_')) --nb;
            std::string var = decl.substr(nb);
            if (var.size() > 2 && var.compare(var.size() - 2, 2, "_0") == 0) var.resize(var.size() - 2);

            const auto& srcLines = GetSourceLines(curFile);
            // #line 只保证起点，附近几行内找声明了该变量且带 vk::binding 的那一行
            std::string srcDecl;
            for (int off : { 0, -1, 1, -2, 2, -3, 3 }) {
                const int idx = srcLine - 1 + off;
                if (idx < 0 || idx >= static_cast<int>(srcLines.size())) continue;
                const std::string& s = srcLines[idx];
                const size_t vp = s.find(var);
                if (vp == std::string::npos) continue;
                const size_t ve = vp + var.size();
                if (ve < s.size() && (isalnum(static_cast<unsigned char>(s[ve])) || s[ve] == '_')) continue;
                srcDecl = s;
                break;
            }
            const size_t vb = srcDecl.find("vk::binding(");
            // 源码已显式写 register(...) 的保持原样
            // cbuffer 只处理源码带 vk::binding 的（描述符集里的常量缓冲）；
            // [[vk::push_constant]] 的 cbuffer 源码里没有 vk::binding，保持自动分配（b0..b7 的 root CBV）
            const bool clsOk = isCBuffer ? (cls == 'b') : (cls == 't' || cls == 'u' || cls == 's');
            if (clsOk && close != std::string::npos &&
                vb != std::string::npos && srcDecl.find("register(") == std::string::npos) {
                const size_t ab = vb + 12;
                const size_t ae = srcDecl.find(')', ab);
                const std::string args = srcDecl.substr(ab, ae - ab);
                const size_t comma = args.find(',');
                int binding = 0, space = 0;
                const bool ok = ResolveInt(args.substr(0, comma), curFile, binding) &&
                    (comma == std::string::npos || ResolveInt(args.substr(comma + 1), curFile, space));
                // 描述符集 CBV 的寄存器从 b8 起（b0..b7 留给推送常量的 root CBV），
                // 与根签名里 SRV 表附带的 CBV 范围（BaseShaderRegister=8）一致
                if (isCBuffer) binding += 8;
                if (ok) {
                    line = line.substr(0, clsPos) + cls + std::to_string(binding) +
                           ", space" + std::to_string(space) + line.substr(close);
                } else {
                    std::fprintf(stderr, "ShaderToDxil: 警告：无法解析 %s 的 vk::binding(%s)，保留自动分配的寄存器\n",
                                 var.c_str(), args.c_str());
                }
            }
        }
        out += line; out += '\n';
    }
    return out;
}

// ---------------------------------------------------------------------------
//  库目标（光追）的 bindless 数组空间归位
//
//  【为什么必须做】D3D12 后端把 bindless 的**纹理堆 / 采样器堆**分别绑在 space2 的
//  SRV 表与采样器表上（见 D3D12CommandList::PrepareSetTables 的 SetRootTable(10/26)）。
//  而 Slang 对「声明在 set0 的无界数组」会自行挪到别的 space（实测 PT_Full.rchit 的
//  `g_MaterialTextures[]` 被放到 space1、采样器放到 space2）：space1 的 SRV 表在
//  D3D12 下根本没绑，命中着色器采样读到全 0 ⇒ PT 的 albedo 全 0、roughness 恒为 0.04、
//  画面整体偏暗（与 Vulkan 对比实测）。这里把库里的无界数组统一改到 space2 的 0 号寄存器，
//  与光栅路径的 bindless 约定一致（t0,space2 / s0,space2）。
// ---------------------------------------------------------------------------
static void PatchUnboundedArraysToBindlessSpace(std::string& s) {
    size_t pos = 0;
    while ((pos = s.find("[]", pos)) != std::string::npos) {
        const size_t reg = s.find(": register(", pos);
        if (reg == std::string::npos) break;
        const size_t eol = s.find('\n', pos);
        if (eol == std::string::npos || reg > eol) { pos += 2; continue; }   // 跨行：不是声明
        const size_t clsPos = reg + 11;                                      // "(" 之后
        const char cls = (clsPos < s.size()) ? s[clsPos] : 0;
        if (cls != 't' && cls != 'u' && cls != 's') { pos += 2; continue; }
        const size_t close = s.find(')', clsPos);
        if (close == std::string::npos) break;
        s.replace(clsPos, close - clsPos, std::string(1, cls) + "0, space2");
        pos = clsPos + 12;
    }
}


/// 网格着色器入口参数修正：Slang 生成的 HLSL 在输出数组参数上多带一个 inout
///（如 `out vertices inout T verts[N]`、`out indices inout uint3 prims[M]`），有时连
/// `out vertices` 都缺失（只剩 `inout T verts[N]`），dxc 报 "duplicate HLSL parameter usages"。
/// 这里只在入口函数的参数列表内修正：去掉多余的 inout，缺修饰的数组参数补成 `out vertices`
static void PatchMeshSignature(std::string& s, const std::string& entry) {
    const std::string key = "void " + entry + "(";
    const size_t start = s.find(key);
    if (start == std::string::npos) return;
    const size_t open  = start + key.size();
    // 按括号配对找参数列表的结尾：数组维度里有 int(N) 这样的括号
    size_t close = std::string::npos;
    for (size_t i = open, depth = 1; i < s.size(); ++i) {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')' && --depth == 0) { close = i; break; }
    }
    if (close == std::string::npos) return;
    std::string params = s.substr(open, close - open);
    ReplaceAll(params, "out vertices inout ",   "out vertices ");
    ReplaceAll(params, "out indices inout ",    "out indices ");
    ReplaceAll(params, "out primitives inout ", "out primitives ");
    // 剩下的 inout 数组参数只可能是顶点输出（图元索引 / 逐图元属性都带了修饰）
    size_t p = 0;
    while ((p = params.find("inout ", p)) != std::string::npos) {
        params.replace(p, 6, "out vertices ");
        p += 13;
    }
    s.replace(open, close - open, params);
}

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
    // 【lib（DXIL 库）】光追着色器（*.rgen/.rmiss/.rchit/.rahit/.rcall/.rint）走库目标：
    //  · 不做 VS/PS 的语义注入（光线追踪着色器没有 VS→PS 链接，强行补 SV_Position 会编译失败）；
    //  · 不加 [loop]（库里的函数体由 dxc 自行决定展开）；
    //  · 不做实例号改写（那是顶点着色器专用）；
    //  · 入口属性 [shader("...")] 由 slangc -target hlsl 直接产出（实测已带），无需补；
    //  · **仍然要**做寄存器重排（register = binding、space = set）。
    const bool isLib = (std::string(stage) == "lib");
    // 目标 profile 必须先定下来：顶点着色器的实例号改写方式取决于着色器模型
    //（SM 6.8 起用系统值 SV_StartInstanceLocation，以下仍用逐实例属性绕行）
    char profile[32];
    if (profileArg) {
        std::snprintf(profile, sizeof(profile), "%s", profileArg);
    } else {
        std::string s = stage;
        // 【T6】顶点着色器提到 vs_6_8：D3D12 的 SV_InstanceID 不含起始实例号，6.8 的
        // SV_StartInstanceLocation 才能在不绑序号缓冲的前提下与 Vulkan 的 gl_InstanceIndex 对齐
        const char* t = (s == "vs") ? "vs_6_8" : (s == "ps") ? "ps_6_6"
                      : (s == "cs") ? "cs_6_6" : (s == "ms") ? "ms_6_6"
                      : (s == "as") ? "as_6_6" : "lib_6_6";
        std::snprintf(profile, sizeof(profile), "%s", t);
    }
    std::string patched = src;
    if (!isLib) {
        patched = InjectSemantics(src, isVertex);
    }
    PatchUint8(patched);
    if (!isLib) {
        // 【已回退】PatchCBuffers 会把所有 cbuffer 归一到 b0，导致多个 cbuffer 重叠
        //（dxc 报 "resource X at register 0 overlaps with resource Y"）。
        // 正确的做法是保留着色器自己的寄存器分配（b0/b1/...），根签名用 root CBV 覆盖 b0..b7。
        PatchLoops(patched);
        if (std::string(stage) == "vs") {
            if (ProfileAtLeast68(profile)) PatchStartInstanceLocation(patched);
            else                           PatchInstanceId(patched);
        }
        if (std::string(stage) == "ms") PatchMeshSignature(patched, entry);   // 修正网格着色器输出参数修饰
    } else if (patched.find("[shader(") == std::string::npos) {
        // 兜底提示：库目标靠 [shader("...")] 导出入口，缺了 dxc 会报 "no entry point"
        std::fprintf(stderr, "ShaderToDxil: 警告：%s 里没有 [shader(...)] 属性，库目标可能编译失败\n", input);
    }
    // 寄存器按 vk::binding 重排，与 D3D12 后端的「register = binding、space = set」约定一致
    patched = PatchRegisters(patched);
    // 库目标（光追）里的无界数组统一归到 space2（D3D12 的 bindless 堆绑在那张表上）
    if (isLib) PatchUnboundedArraysToBindlessSpace(patched);

    // 落一份补丁后的 HLSL 便于排查（设 HE_DUMP_DXIL_HLSL=path 覆盖默认名）
    if (const char* dump = std::getenv("HE_DUMP_DXIL_HLSL")) {
        WriteFile(dump, patched);
    }

    const std::string hlslPath = std::string(output) + ".hlsl";
    if (!WriteFile(hlslPath.c_str(), patched)) {
        std::fprintf(stderr, "ShaderToDxil: 无法写临时 HLSL %s\n", hlslPath.c_str());
        return 1;
    }

    // 调用 dxc
    const std::string dxilPath = std::string(output) + ".dxil";
    std::string cmd = "dxc.exe -T ";
    cmd += profile;
    // 【库目标没有单一入口】lib_6_6 下 dxc 不能带 -E（会报 "-E not supported for lib target"）：
    // 整个编译单元就是一个库，入口由源码里的 [shader("...")] 属性标记
    if (!isLib) {
        cmd += " -E ";
        cmd += entry;
    }
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
