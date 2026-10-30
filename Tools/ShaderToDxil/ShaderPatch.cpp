// ============================================================================
//  ShaderPatch.cpp —— HLSL 文本补丁实现（说明见 ShaderPatch.h）
// ============================================================================

#include "ShaderPatch.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace shaderpatch {

using he::rhi::dxbc::DxbcBindEntry;
using he::rhi::dxbc::DxbcBindType;

// ---------------------------------------------------------------------------
//  文件读取（寄存器补丁要回读 Slang 源码里的 vk::binding）
// ---------------------------------------------------------------------------
static bool ReadFile(const char* path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
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

std::string InjectSemantics(std::string src, bool isVertexStage) {
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

void PatchUint8(std::string& s) {
    ReplaceAll(s, "vector<uint8_t,", "vector<uint,");
    ReplaceAll(s, "uint8_t", "uint");
}

/// 顶点着色器的实例号改为读逐实例顶点属性 HE_INSTANCEINDEX。
/// Vulkan 的 gl_InstanceIndex 含 firstInstance（GPU 剔除把对象号、Nanite 把簇号放在 firstInstance 里），
/// 而 D3D12 的 SV_InstanceID 恒从 0 起、不含 StartInstanceLocation（SM 6.8 才有 SV_StartInstanceLocation）。
/// 但 IA 取逐实例数据时会加上 StartInstanceLocation：后端在专用输入槽绑定内容为 0,1,2… 的逐实例缓冲，
/// 读出的值即「起始实例 + 实例号」，直接绘制与间接绘制都与 Vulkan 一致（见 D3D12PipelineState::kInstanceIndexSlot）
/// D3D11 的 SV_InstanceID 同样不含起始实例，做法相同。
void PatchInstanceId(std::string& s) {
    ReplaceAll(s, ": SV_InstanceID", ": HE_INSTANCEINDEX");
}

/// 给 for 循环加 [loop]：抑制 DXC 的激进展开（某些循环展开后编译时间爆炸）
void PatchLoops(std::string& s) {
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
    // 「常量 + 偏移」形式（如 kGPUBinding_NB_MaterialTextures + 1）：逐项解析后求和
    if (const size_t plus = t.find('+'); plus != std::string::npos) {
        int a = 0, b = 0;
        if (!ResolveInt(t.substr(0, plus), srcPath, a) || !ResolveInt(t.substr(plus + 1), srcPath, b)) return false;
        out = a + b;
        return true;
    }
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

/// 从一行 HLSL 声明（register 之前的部分）取变量名：最后一个标识符，去掉 Slang 加的 `_0` 后缀和数组维度
static std::string DeclVarName(const std::string& line, size_t reg) {
    std::string decl = line.substr(0, reg);
    const size_t br = decl.find('[');
    if (br != std::string::npos) decl.resize(br);
    decl = Trim(decl);
    size_t nb = decl.size();
    while (nb > 0 && (isalnum(static_cast<unsigned char>(decl[nb - 1])) || decl[nb - 1] == '_')) --nb;
    std::string var = decl.substr(nb);
    if (var.size() > 2 && var.compare(var.size() - 2, 2, "_0") == 0) var.resize(var.size() - 2);
    return var;
}

/// #line 只保证起点，在源码附近几行内找声明了该变量的那一行（找不到返回空串）
static std::string FindSourceDecl(const std::string& curFile, int srcLine, const std::string& var) {
    const auto& srcLines = GetSourceLines(curFile);
    for (int off : { 0, -1, 1, -2, 2, -3, 3 }) {
        const int idx = srcLine - 1 + off;
        if (idx < 0 || idx >= static_cast<int>(srcLines.size())) continue;
        const std::string& s = srcLines[idx];
        const size_t vp = s.find(var);
        if (vp == std::string::npos) continue;
        const size_t ve = vp + var.size();
        if (ve < s.size() && (isalnum(static_cast<unsigned char>(s[ve])) || s[ve] == '_')) continue;
        return s;
    }
    return {};
}

/// 跟踪 `#line N "file"`：返回 true 表示这一行是 #line 指令（已更新 curFile / nextLine）
static bool TrackLineDirective(const std::string& line, std::string& curFile, int& nextLine) {
    if (line.rfind("#line ", 0) != 0) return false;
    // 形如 `#line 38 "path"` 或 `#line 76`（沿用上一个文件）
    std::istringstream ls(line.substr(6));
    ls >> nextLine;
    const size_t q1 = line.find('"');
    const size_t q2 = (q1 == std::string::npos) ? q1 : line.find('"', q1 + 1);
    if (q2 != std::string::npos) curFile = line.substr(q1 + 1, q2 - q1 - 1);
    return true;
}

/// 解析源码声明里 `vk::binding(B[, S])` 的参数（S 缺省为 0）
static bool ParseVkBinding(const std::string& srcDecl, const std::string& curFile,
                           int& binding, int& space, std::string& argsOut) {
    const size_t vb = srcDecl.find("vk::binding(");
    if (vb == std::string::npos) return false;
    const size_t ab = vb + 12;
    const size_t ae = srcDecl.find(')', ab);
    argsOut = srcDecl.substr(ab, ae - ab);
    const size_t comma = argsOut.find(',');
    binding = 0; space = 0;
    return ResolveInt(argsOut.substr(0, comma), curFile, binding) &&
           (comma == std::string::npos || ResolveInt(argsOut.substr(comma + 1), curFile, space));
}

std::string PatchRegisters(const std::string& src) {
    std::istringstream in(src);
    std::string out, line;
    std::string curFile;
    int nextLine = 0;   // 下一行 HLSL 对应的源码行号（0 = 未知）
    while (std::getline(in, line)) {
        if (TrackLineDirective(line, curFile, nextLine)) {
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
            const std::string var = DeclVarName(line, reg);
            const std::string srcDecl = FindSourceDecl(curFile, srcLine, var);
            const size_t vb = srcDecl.find("vk::binding(");
            // 源码已显式写 register(...) 的保持原样
            // cbuffer 只处理源码带 vk::binding 的（描述符集里的常量缓冲）；
            // [[vk::push_constant]] 的 cbuffer 源码里没有 vk::binding，保持自动分配（b0..b7 的 root CBV）
            const bool clsOk = isCBuffer ? (cls == 'b') : (cls == 't' || cls == 'u' || cls == 's');
            if (clsOk && close != std::string::npos &&
                vb != std::string::npos && srcDecl.find("register(") == std::string::npos) {
                int binding = 0, space = 0;
                std::string args;
                const bool ok = ParseVkBinding(srcDecl, curFile, binding, space, args);
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

// ===========================================================================
//  D3D11（DXBC）专用
// ===========================================================================

static bool IsIdentChar(char c) {
    return isalnum(static_cast<unsigned char>(c)) || c == '_';
}

/// 删除所有 `[unroll]` / `[unroll(N)]` 属性
static void StripUnroll(std::string& s) {
    size_t p = 0;
    while ((p = s.find("[unroll", p)) != std::string::npos) {
        const size_t after = p + 7;
        if (after < s.size() && (s[after] == ']' || s[after] == '(')) {
            const size_t close = s.find(']', after);
            if (close == std::string::npos) break;
            size_t end = close + 1;
            while (end < s.size() && (s[end] == ' ' || s[end] == '\t')) ++end;
            s.erase(p, end - p);
        } else {
            p = after;
        }
    }
}

/// 关键字前（跳过空白）是否已有 `[loop]` 属性
static bool PrecededByLoopAttr(const std::string& s, size_t kw) {
    size_t i = kw;
    while (i > 0 && isspace(static_cast<unsigned char>(s[i - 1]))) --i;
    if (i == 0 || s[i - 1] != ']') return false;
    const size_t open = s.rfind('[', i - 1);
    return open != std::string::npos && s.compare(open, 6, "[loop]") == 0;
}

/// 文本位置 pos 在 Slang 源码里的位置：往前找最近的 `#line N ["file"]`，行号 = N + 其后的换行数；
/// 不带文件名的 #line 沿用更早一条带文件名的
static LoopSite SourceSiteAt(const std::string& s, size_t pos) {
    LoopSite site;
    size_t dir = std::string::npos;
    for (size_t p = pos; p != std::string::npos && p > 0;) {
        const size_t found = s.rfind("#line ", p - 1);
        if (found == std::string::npos) break;
        if (found == 0 || s[found - 1] == '\n') {
            const size_t eol = s.find('\n', found);
            const std::string directive = s.substr(found, eol - found);
            if (dir == std::string::npos) {
                dir = found;
                site.line = std::atoi(directive.c_str() + 6);
                for (size_t k = eol + 1; k < pos && k < s.size(); ++k) if (s[k] == '\n') ++site.line;
            }
            const size_t q1 = directive.find('"');
            const size_t q2 = (q1 == std::string::npos) ? q1 : directive.find('"', q1 + 1);
            if (q2 != std::string::npos) { site.file = directive.substr(q1 + 1, q2 - q1 - 1); break; }
        }
        p = found;
    }
    return site;
}

/// 两个源码路径是否指同一文件（fxc 报错与 #line 的写法可能一个用正斜杠、一个用反斜杠）
static bool SameSourceFile(std::string a, std::string b) {
    for (auto* s : { &a, &b }) for (char& c : *s) { if (c == '\\') c = '/'; c = static_cast<char>(tolower(static_cast<unsigned char>(c))); }
    return a == b;
}

void PatchLoopsFxc(std::string& s, bool insertLoop, const std::vector<LoopSite>* skip) {
    StripUnroll(s);
    if (!insertLoop) return;
    // 该位置的循环是否在 skip 列表里（同文件、行号相差 ≤ 2：fxc 报的行号可能是循环条件所在行）
    auto skipped = [&](size_t pos) {
        if (!skip || skip->empty()) return false;
        const LoopSite site = SourceSiteAt(s, pos);
        for (const LoopSite& k : *skip) {
            if (std::abs(k.line - site.line) <= 2 && SameSourceFile(k.file, site.file)) return true;
        }
        return false;
    };

    // 扫描大括号嵌套：栈里记录每个 '{' 是否是循环体；循环深度 = 栈中循环体的个数。
    std::vector<bool> braceIsLoop;
    int    loopDepth       = 0;
    bool   expectLoopBody  = false;     // 刚读完 for(...) / while(...) / do，下一个 '{' 是循环体
    std::vector<size_t> insertAt;       // 需要插入 [loop] 的位置（关键字起点）

    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const char c = s[i];
        // 行注释 / 块注释 / 预处理行（#line 等）整段跳过
        if (c == '/' && i + 1 < n && s[i + 1] == '/') { i = s.find('\n', i); if (i == std::string::npos) break; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') { i = s.find("*/", i + 2); if (i == std::string::npos) break; i += 2; continue; }
        if (c == '#' && (i == 0 || s[i - 1] == '\n')) { i = s.find('\n', i); if (i == std::string::npos) break; continue; }

        if (IsIdentChar(c) && (i == 0 || !IsIdentChar(s[i - 1]))) {
            size_t e = i;
            while (e < n && IsIdentChar(s[e])) ++e;
            const std::string word = s.substr(i, e - i);
            if (word == "for" || word == "while" || word == "do") {
                size_t j = e;
                while (j < n && isspace(static_cast<unsigned char>(s[j]))) ++j;
                bool isLoop = (word == "do");
                size_t resume = e;
                if (!isLoop && j < n && s[j] == '(') {
                    // 跳过括号里的循环头
                    int paren = 0; size_t k = j;
                    for (; k < n; ++k) {
                        if (s[k] == '(') ++paren;
                        else if (s[k] == ')' && --paren == 0) break;
                    }
                    size_t after = k + 1;
                    while (after < n && isspace(static_cast<unsigned char>(s[after]))) ++after;
                    // `} while(cond);` 是 do-while 的尾巴，不是新循环
                    isLoop = !(word == "while" && after < n && s[after] == ';');
                    resume = k + 1;
                }
                if (isLoop) {
                    if (loopDepth == 0 && !PrecededByLoopAttr(s, i) && !skipped(i)) insertAt.push_back(i);
                    expectLoopBody = true;
                }
                i = resume;
                continue;
            }
            i = e;
            continue;
        }

        if (c == '{') {
            braceIsLoop.push_back(expectLoopBody);
            if (expectLoopBody) ++loopDepth;
            expectLoopBody = false;
        } else if (c == '}') {
            if (!braceIsLoop.empty()) {
                if (braceIsLoop.back()) --loopDepth;
                braceIsLoop.pop_back();
            }
        } else if (expectLoopBody && !isspace(static_cast<unsigned char>(c))) {
            // 不带大括号的单语句循环体：不入栈（其内部的循环不会被视为最外层）
            expectLoopBody = false;
        }
        ++i;
    }

    // 从后往前插入，前面的位置不受影响
    for (auto it = insertAt.rbegin(); it != insertAt.rend(); ++it) s.insert(*it, "[loop] ");
}

/// 解析 HLSL 声明里的数组元素个数：`x[int(256)]` / `x[256]` → 256；非数组 → 1；无界 `x[]` → 0
static uint32_t ParseArrayCount(const std::string& declPart) {
    const size_t br = declPart.find('[');
    if (br == std::string::npos) return 1;
    size_t k = br + 1;
    while (k < declPart.size() && !isdigit(static_cast<unsigned char>(declPart[k])) && declPart[k] != ']') ++k;
    if (k >= declPart.size() || declPart[k] == ']') return 0;
    return static_cast<uint32_t>(std::strtoul(declPart.c_str() + k, nullptr, 10));
}

void PatchBranchesFxc(std::string& s) {
    std::vector<size_t> insertAt;
    for (size_t p = 0; (p = s.find("if", p)) != std::string::npos; p += 2) {
        // 必须是独立的关键字 if，后面（跳过空白）紧跟 '('
        if (p > 0 && (IsIdentChar(s[p - 1]) || s[p - 1] == '#')) continue;
        if (p + 2 < s.size() && IsIdentChar(s[p + 2])) continue;
        size_t k = p + 2;
        while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) ++k;
        if (k >= s.size() || s[k] != '(') continue;
        // 往前看：已有属性（`]`）或是 else if 的就跳过
        size_t b = p;
        while (b > 0 && isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        if (b > 0 && s[b - 1] == ']') continue;
        if (b >= 4 && s.compare(b - 4, 4, "else") == 0 && (b == 4 || !IsIdentChar(s[b - 5]))) continue;
        insertAt.push_back(p);
    }
    // 从后往前插入，前面的位置不受影响
    for (auto it = insertAt.rbegin(); it != insertAt.rend(); ++it) s.insert(*it, "[branch] ");
}

// ---------------------------------------------------------------------------
//  结构化缓冲里的矩阵（fxc 一律按行主序读，见 ShaderPatch.h）
// ---------------------------------------------------------------------------

/// 类型名是否为矩阵（float4x4 / uint3x3 …）；是则给出行列数
static bool ParseMatrixType(const std::string& type, int& rows, int& cols) {
    static const char* kScalars[] = { "float", "half", "double", "int", "uint", "min16float", "bool" };
    for (const char* sc : kScalars) {
        const size_t n = std::strlen(sc);
        if (type.size() == n + 3 && type.compare(0, n, sc) == 0 &&
            isdigit(static_cast<unsigned char>(type[n])) && type[n + 1] == 'x' &&
            isdigit(static_cast<unsigned char>(type[n + 2]))) {
            rows = type[n] - '0';
            cols = type[n + 2] - '0';
            return true;
        }
    }
    return false;
}

std::vector<std::string> PatchStructuredMatricesFxc(std::string& s) {
    std::vector<std::string> problems;

    // 1) 收集结构体成员：名字 → [(类型, 成员名, 数组长度)]（Slang 输出的结构体不嵌套花括号）
    struct Member { std::string type, name; uint32_t count = 1; };
    std::map<std::string, std::vector<Member>> structs;
    std::vector<std::string> structOrder;   // 定义顺序（嵌套结构总是先定义）
    std::map<std::string, size_t> structEnd; // 结构体定义末尾（`};` 之后）的位置：修正函数插在这里
    for (size_t p = 0; (p = s.find("struct ", p)) != std::string::npos; p += 7) {
        if (p > 0 && IsIdentChar(s[p - 1])) continue;
        size_t k = p + 7;
        while (k < s.size() && isspace(static_cast<unsigned char>(s[k]))) ++k;
        const size_t nameBeg = k;
        while (k < s.size() && IsIdentChar(s[k])) ++k;
        const std::string name = s.substr(nameBeg, k - nameBeg);
        while (k < s.size() && isspace(static_cast<unsigned char>(s[k]))) ++k;
        if (name.empty() || k >= s.size() || s[k] != '{') continue;
        const size_t close = s.find('}', k);
        if (close == std::string::npos) break;
        std::vector<Member> members;
        std::istringstream body(s.substr(k + 1, close - k - 1));
        std::string stmt;
        while (std::getline(body, stmt, ';')) {
            // 去掉语义（`: TEXCOORD0`），取最后两个记号：类型 名字[数组]
            const size_t colon = stmt.find(':');
            if (colon != std::string::npos) stmt.resize(colon);
            std::istringstream toks(stmt);
            std::vector<std::string> t;
            for (std::string w; toks >> w;) t.push_back(w);
            // 数组长度写成 `[int(3)]` 时中间无空格；有空格时把尾部拼回名字
            while (t.size() > 2 && t.back().find('[') == std::string::npos && t.back().find(']') != std::string::npos) {
                t[t.size() - 2] += t.back();
                t.pop_back();
            }
            if (t.size() < 2) continue;
            Member m;
            m.type = t[t.size() - 2];
            const std::string& decl = t.back();
            m.name  = decl.substr(0, decl.find('['));
            m.count = ParseArrayCount(decl);
            members.push_back(m);
        }
        structs[name] = std::move(members);
        structOrder.push_back(name);
        size_t endPos = close + 1;
        if (endPos < s.size() && s[endPos] == ';') ++endPos;
        structEnd[name] = endPos;
    }

    // 2) 哪些结构体需要修正（直接或经嵌套成员含方阵）
    std::map<std::string, bool> needFix;
    for (const std::string& name : structOrder) {
        bool need = false;
        for (const Member& m : structs[name]) {
            int r = 0, c = 0;
            if (ParseMatrixType(m.type, r, c)) need = true;
            else if (needFix.count(m.type) && needFix[m.type]) need = true;
        }
        needFix[name] = need;
    }

    // 3) 找只读结构化缓冲的声明：元素是矩阵或需修正的结构体
    struct Buffer { std::string name, elem; };
    std::vector<Buffer> buffers;
    for (size_t p = 0; (p = s.find("StructuredBuffer<", p)) != std::string::npos; p += 17) {
        const bool rw = p >= 2 && s.compare(p - 2, 2, "RW") == 0;
        const bool rasterOrdered = p >= 16 && s.compare(p - 16, 16, "RasterizerOrdered") == 0;
        if (!rw && !rasterOrdered && p > 0 && IsIdentChar(s[p - 1])) continue;   // 如 AppendStructuredBuffer
        const size_t gt = s.find('>', p);
        if (gt == std::string::npos) break;
        std::string elem = Trim(s.substr(p + 17, gt - p - 17));
        size_t k = gt + 1;
        while (k < s.size() && isspace(static_cast<unsigned char>(s[k]))) ++k;
        const size_t nameBeg = k;
        while (k < s.size() && IsIdentChar(s[k])) ++k;
        const std::string name = s.substr(nameBeg, k - nameBeg);
        int r = 0, c = 0;
        const bool isMat = ParseMatrixType(elem, r, c);
        const bool isFixStruct = needFix.count(elem) && needFix[elem];
        if (name.empty() || (!isMat && !isFixStruct)) continue;
        if (rw || rasterOrdered) {
            problems.push_back("可写结构化缓冲 " + name + " 的元素含矩阵（fxc 按行主序读写，未修正）");
            continue;
        }
        if (isMat && r != c) {
            problems.push_back("结构化缓冲 " + name + " 的元素是非方阵 " + elem + "（无法用 transpose 修正）");
            continue;
        }
        buffers.push_back({ name, elem });
    }
    if (buffers.empty()) return problems;

    // 4) 只为缓冲用到的结构体（含嵌套）生成修正函数
    std::map<std::string, bool> used;
    std::vector<std::string> pending;
    for (const Buffer& b : buffers) pending.push_back(b.elem);
    while (!pending.empty()) {
        const std::string t = pending.back();
        pending.pop_back();
        if (!needFix.count(t) || !needFix[t] || used[t]) continue;
        used[t] = true;
        for (const Member& m : structs[t]) pending.push_back(m.type);
    }
    // 修正函数写在结构体定义的 `};` 之后、同一行（不改变行号）；嵌套结构体先定义，函数也先出现
    std::vector<std::pair<size_t, std::string>> funcs;   // (插入位置, 函数文本)
    for (const std::string& name : structOrder) {
        if (!used[name]) continue;
        std::string body;
        for (const Member& m : structs[name]) {
            int r = 0, c = 0;
            std::string fn;
            if (ParseMatrixType(m.type, r, c)) {
                if (r != c) {
                    problems.push_back("结构体 " + name + " 的成员 " + m.name + " 是非方阵 " + m.type + "（未修正）");
                    continue;
                }
                fn = "transpose";
            } else if (needFix.count(m.type) && needFix[m.type]) {
                fn = "HE_FxcMat_" + m.type;
            } else {
                continue;
            }
            if (m.count <= 1) {
                body += " v." + m.name + " = " + fn + "(v." + m.name + ");";
            } else {
                for (uint32_t i = 0; i < m.count; ++i) {
                    const std::string e = "v." + m.name + "[" + std::to_string(i) + "]";
                    body += " " + e + " = " + fn + "(" + e + ");";
                }
            }
        }
        funcs.push_back({ structEnd[name], " " + name + " HE_FxcMat_" + name + "(" + name + " v) {" + body + " return v; }" });
    }

    // 5) 包装访问：`name.Load(...)` / `name[...]` → `修正(…)`。先收集再从后往前改，位置不互相影响
    struct Wrap { size_t beg; std::string fn; };   // 访问表达式的起点 + 修正函数名
    std::vector<Wrap> wraps;
    for (const Buffer& b : buffers) {
        int r = 0, c = 0;
        const std::string fn = ParseMatrixType(b.elem, r, c) ? "transpose" : "HE_FxcMat_" + b.elem;
        for (size_t p = 0; (p = s.find(b.name, p)) != std::string::npos; p += b.name.size()) {
            if (p > 0 && IsIdentChar(s[p - 1])) continue;
            size_t k = p + b.name.size();
            if (k < s.size() && IsIdentChar(s[k])) continue;
            if (s.compare(k, 6, ".Load(") != 0 && !(k < s.size() && s[k] == '[')) continue;
            wraps.push_back({ p, fn });
        }
    }
    // 外层包装可能含内层（如 a.Load(b.Load(i).x)）：按起点从后往前插入，内层先改不影响外层的起点
    std::sort(wraps.begin(), wraps.end(), [](const Wrap& a, const Wrap& b) { return a.beg > b.beg; });
    for (const Wrap& w : wraps) {
        // 内层插入后外层的终点会后移：此时才按括号匹配找终点
        size_t k = w.beg;
        while (k < s.size() && IsIdentChar(s[k])) ++k;
        const char open = s[k] == '[' ? '[' : '(';
        const char closeCh = open == '[' ? ']' : ')';
        if (open == '(') k += 5;   // 跳过 ".Load"
        int depth = 0;
        size_t e = k;
        for (; e < s.size(); ++e) {
            if (s[e] == open) ++depth;
            else if (s[e] == closeCh && --depth == 0) break;
        }
        if (e >= s.size()) continue;
        s.insert(e + 1, ")");
        s.insert(w.beg, w.fn + "(");
    }
    // 结构体定义在所有访问之前，上面的改动不影响这些位置；从后往前插入
    for (auto it = funcs.rbegin(); it != funcs.rend(); ++it) s.insert(it->first, it->second);
    return problems;
}

static const char* BindTypeName(DxbcBindType t) {
    switch (t) {
        case DxbcBindType::SRV:          return "SRV";
        case DxbcBindType::UAV:          return "UAV";
        case DxbcBindType::Sampler:      return "采样器";
        case DxbcBindType::CBuffer:      return "常量缓冲";
        case DxbcBindType::PushConstant: return "推送常量";
    }
    return "?";
}

D3D11RegisterResult PatchRegistersD3D11(const std::string& src, bool isPixelStage) {
    using namespace he::rhi::dxbc;
    D3D11RegisterResult result;

    std::istringstream in(src);
    std::string line;
    std::string curFile;
    int nextLine = 0;
    uint32_t nextSampler = 0;                       // 采样器按声明顺序压缩
    uint32_t nextCBuffer = kDxbcFirstCBufferSlot;   // 描述符集常量缓冲从 b1 起压缩
    bool     havePush    = false;

    while (std::getline(in, line)) {
        if (TrackLineDirective(line, curFile, nextLine)) {
            result.hlsl += line; result.hlsl += '\n';
            continue;
        }
        const int srcLine = nextLine;
        if (nextLine > 0) ++nextLine;

        const size_t reg = line.find(": register(");
        if (reg == std::string::npos) { result.hlsl += line; result.hlsl += '\n'; continue; }

        const size_t clsPos = reg + 11;
        const char   cls    = (clsPos < line.size()) ? line[clsPos] : 0;
        const size_t close  = line.find(')', clsPos);
        const std::string var     = DeclVarName(line, reg);
        const std::string srcDecl = (srcLine > 0 && !curFile.empty()) ? FindSourceDecl(curFile, srcLine, var) : std::string();
        const uint32_t count = ParseArrayCount(line.substr(0, reg));

        auto fail = [&](const std::string& why) {
            result.errors.push_back(var + "：" + why);
        };

        if (close == std::string::npos || (cls != 't' && cls != 'u' && cls != 's' && cls != 'b')) {
            fail("无法识别的寄存器声明");
            result.hlsl += line; result.hlsl += '\n';
            continue;
        }
        if (count == 0) {
            fail("无界资源数组，SM 5.0 不支持");
            result.hlsl += line; result.hlsl += '\n';
            continue;
        }

        DxbcBindEntry entry;
        entry.count = count;
        int binding = 0, space = 0;
        std::string args;
        const bool hasBinding = ParseVkBinding(srcDecl, curFile, binding, space, args);

        if (cls == 'b' && !hasBinding) {
            // 没有 vk::binding 的常量缓冲只可能是 [[vk::push_constant]] 块：固定 b0
            if (havePush) {
                fail("出现第二个推送常量块（或常量缓冲缺少 vk::binding）");
            }
            havePush = true;
            entry.type = static_cast<uint32_t>(DxbcBindType::PushConstant);
            entry.slot = kDxbcPushConstantSlot;
        } else if (!hasBinding) {
            fail(args.empty() ? "源码里找不到 vk::binding" : "无法解析 vk::binding(" + args + ")");
            result.hlsl += line; result.hlsl += '\n';
            continue;
        } else {
            entry.set     = static_cast<uint32_t>(space);
            entry.binding = static_cast<uint32_t>(binding);
            switch (cls) {
                case 't':
                    entry.type = static_cast<uint32_t>(DxbcBindType::SRV);
                    entry.slot = static_cast<uint32_t>(binding);
                    if (entry.slot + count > kDxbcMaxSRVSlots) fail("SRV 槽位越界（t" + std::to_string(entry.slot) + " × " + std::to_string(count) + "）");
                    break;
                case 'u':
                    entry.type = static_cast<uint32_t>(DxbcBindType::UAV);
                    entry.slot = static_cast<uint32_t>(binding) + (isPixelStage ? kDxbcPixelUAVBase : 0u);
                    if (entry.slot + count > kDxbcMaxUAVSlots) fail("UAV 槽位越界（u" + std::to_string(entry.slot) + " × " + std::to_string(count) + "）");
                    break;
                case 's':
                    entry.type = static_cast<uint32_t>(DxbcBindType::Sampler);
                    entry.slot = nextSampler;
                    nextSampler += count;
                    if (nextSampler > kDxbcMaxSamplerSlots) fail("采样器超过 16 个");
                    break;
                default:  // 'b'
                    entry.type = static_cast<uint32_t>(DxbcBindType::CBuffer);
                    entry.slot = nextCBuffer;
                    nextCBuffer += count;
                    if (nextCBuffer > kDxbcMaxCBufferSlots) fail("常量缓冲超过 13 个（b0 留给推送常量）");
                    break;
            }
        }

        // 同类寄存器的区间不得重叠
        for (const auto& other : result.bindings) {
            if (other.type == entry.type ||
                (cls == 'b' && (other.type == static_cast<uint32_t>(DxbcBindType::CBuffer) ||
                                other.type == static_cast<uint32_t>(DxbcBindType::PushConstant)))) {
                if (entry.slot < other.slot + other.count && other.slot < entry.slot + entry.count) {
                    fail(std::string(BindTypeName(static_cast<DxbcBindType>(entry.type))) + " 槽位 " +
                         std::to_string(entry.slot) + " 与已分配的 (set " + std::to_string(other.set) +
                         ", binding " + std::to_string(other.binding) + ") 重叠");
                    break;
                }
            }
        }

        // 整个 register(...) 换成不带 space 的 D3D11 寄存器（源码显式写的 spaceN 一并丢弃）
        line = line.substr(0, clsPos) + cls + std::to_string(entry.slot) + line.substr(close);
        result.bindings.push_back(entry);
        result.hlsl += line; result.hlsl += '\n';
    }
    return result;
}

std::vector<unsigned char> SerializeBindTable(const std::vector<DxbcBindEntry>& bindings) {
    he::rhi::dxbc::DxbcBindTableHeader header;
    header.entryCount = static_cast<uint32_t>(bindings.size());
    std::vector<unsigned char> bytes(sizeof(header) + bindings.size() * sizeof(DxbcBindEntry));
    std::memcpy(bytes.data(), &header, sizeof(header));
    if (!bindings.empty()) {
        std::memcpy(bytes.data() + sizeof(header), bindings.data(), bindings.size() * sizeof(DxbcBindEntry));
    }
    return bytes;
}

} // namespace shaderpatch
