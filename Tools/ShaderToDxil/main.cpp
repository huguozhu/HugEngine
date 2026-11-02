// ============================================================================
//  ShaderToDxil —— D3D 后端的着色器构建工具
//
//  职责：把 `slangc -target hlsl` 输出的 HLSL 文本处理成编译器能吃、且语义完整
//        的 HLSL，再编译成字节码，最后写成一个 C++ 头文件（字节数组）。
//    · 默认模式（D3D12）：调用 dxc 生成 DXIL（SM 6.6；顶点着色器用 6.8）。
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
//  补丁的落点：D3D12 与 D3D11 共用的部分（语义注入 / uint8_t / 实例号）以及 D3D11 专属
//  部分（fxc 的循环与分支属性、寄存器重分配、绑定表）在 ShaderPatch.h / ShaderPatch.cpp；
//  D3D12 专属部分（SM 6.8 起始实例号、网格着色器签名、库目标 bindless 归位）留在本文件。
//
//  用法：
//    ShaderToDxil [--dxbc] <input.hlsl> <entry> <stage> <output.h> <symbol> [profile]
//    stage: vs / ps / cs / ms / as / lib（--dxbc 只支持 vs / ps / cs）
//    profile 缺省：vs→vs_6_8（T6：SV_StartInstanceLocation 需要 6.8），
//                  其余 ps/cs/ms/as→6_6，lib→lib_6_6
// ============================================================================

#include "ShaderPatch.h"

#include <windows.h>
#include <d3dcompiler.h>   // D3DCompile：进程内生成 DXBC（d3dcompiler_47）

#include <cctype>
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
//  文本替换小工具（本文件的 D3D12 专属补丁用；共用补丁的同名工具在 ShaderPatch.cpp）
// ---------------------------------------------------------------------------
static void ReplaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) {
        s.replace(p, from.size(), to);
        p += to.size();
    }
}

// ---------------------------------------------------------------------------
//  顶点着色器的实例号（D3D12）：SM 6.8 起用 SV_StartInstanceLocation
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

    // 补丁（insertLoop 决定是否给最外层循环加 [loop]）+ 寄存器重新分配；寄存器问题直接失败
    const std::string hlslPath = std::string(output) + ".hlsl";
    std::vector<LoopSite> noLoopSites;   // fxc 报 X3531 的循环：这些位置不加 [loop]
    bool forceBranch = false;            // fxc 报 flattened side effect 后：所有 if 加 [branch] 重编
    auto buildPatched = [&](bool insertLoop, D3D11RegisterResult& regs) -> bool {
        std::string patched = InjectSemantics(src, st == "vs");
        PatchUint8(patched);
        PatchLoopsFxc(patched, insertLoop, &noLoopSites);
        if (forceBranch) PatchBranchesFxc(patched);
        if (st == "vs") PatchInstanceId(patched);
        // fxc 按行主序读结构化缓冲里的矩阵：读取处补 transpose（无法修正的情况只告警）
        for (const std::string& p : PatchStructuredMatricesFxc(patched)) {
            if (insertLoop) std::fprintf(stderr, "ShaderToDxil --dxbc: %s 警告：%s\n", input, p.c_str());
        }
        regs = PatchRegistersD3D11(patched, st == "ps");
        if (!WriteFile(hlslPath.c_str(), regs.hlsl)) {
            std::fprintf(stderr, "ShaderToDxil: 无法写临时 HLSL %s\n", hlslPath.c_str());
            return false;
        }
        if (!regs.errors.empty()) {
            std::fprintf(stderr, "ShaderToDxil --dxbc: %s 无法用于 D3D11（%zu 处寄存器问题）：\n",
                         input, regs.errors.size());
            for (const auto& e : regs.errors) std::fprintf(stderr, "    %s\n", e.c_str());
            return false;
        }
        return true;
    };
    // 进程内调用 fxc（d3dcompiler_47）。
    // 源文件名传补丁后的 HLSL 路径；HLSL 里保留了 Slang 生成的 `#line N "xxx.slang"`，
    // 所以报错位置直接是 Slang 源文件与行号，不用再人工对照中间 HLSL。
    // 不加 D3DCOMPILE_ENABLE_STRICTNESS：严格模式会强制展开带 break 的循环（X3511）。
    const std::string profile = st + "_5_0";
    std::string diagnostics;   // 本次编译的错误 / 警告文本
    auto compile = [&](const std::string& hlsl, ID3DBlob*& code) -> bool {
        ID3DBlob* errors = nullptr;
        code = nullptr;
        const HRESULT hr = D3DCompile(hlsl.data(), hlsl.size(), hlslPath.c_str(), nullptr, nullptr, entry,
                                      profile.c_str(), D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        diagnostics = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "";
        if (errors) errors->Release();
        if (FAILED(hr) || !code) {
            if (code) { code->Release(); code = nullptr; }
            return false;
        }
        return true;
    };

    // 从 fxc 的报错里取出所有 X3531 的位置（形如 `path(119,16-22): error X3531: ...`）
    auto x3531Sites = [&]() {
        std::vector<LoopSite> sites;
        std::istringstream lines(diagnostics);
        std::string l;
        while (std::getline(lines, l)) {
            if (l.find("X3531") == std::string::npos) continue;
            const size_t colon = l.find("): error");
            const size_t paren = (colon == std::string::npos) ? colon : l.rfind('(', colon);
            if (paren == std::string::npos) continue;
            sites.push_back({ l.substr(0, paren), std::atoi(l.c_str() + paren + 1) });
        }
        return sites;
    };

    D3D11RegisterResult regs;
    ID3DBlob* code = nullptr;
    bool ok = false;
    // 整个重试流程最多跑两轮：第二轮只在第一轮报 flattened side effect 或 X3694 时进行（所有 if 加 [branch]）。
    // 两者都是 fxc 把带 UAV 写入的分支展平造成的：前者无法展平而内部报错，后者展平后
    // `if (线程号 == 0) 写同一地址` 变成所有线程都写，被判为写竞争
    for (int round = 0; round < 2 && !ok; ++round) {
        if (round == 1) {
            if (diagnostics.find("flattened side effect") == std::string::npos &&
                diagnostics.find("X3694") == std::string::npos) break;
            std::fprintf(stderr, "ShaderToDxil --dxbc: %s 报 flattened side effect / X3694，所有 if 加 [branch] 后重编\n", input);
            forceBranch = true;
            noLoopSites.clear();
        }
        // 【逐循环重试】给最外层循环加 [loop] 是为了挡住 fxc 对动态边界循环的强制展开（X3511）；
        // 但循环体里有隐式导数采样（Sample）且处在分支内的定长循环，fxc 必须展开它才能算导数，
        // [loop] 反而让它报 X3531。每次把报 X3531 的循环位置加入「不加 [loop]」名单重编，
        // 其余循环保留 [loop]，直到编过或不再出现新的 X3531 位置
        for (int attempt = 0; attempt < 16; ++attempt) {
            if (!buildPatched(true, regs)) return 1;
            ok = compile(regs.hlsl, code);
            if (ok) break;
            const std::vector<LoopSite> sites = x3531Sites();
            size_t added = 0;
            for (const LoopSite& site : sites) {
                bool known = false;
                for (const LoopSite& k : noLoopSites) known = known || (k.line == site.line && k.file == site.file);
                if (!known) { noLoopSites.push_back(site); ++added; }
            }
            if (added == 0) break;
            std::fprintf(stderr, "ShaderToDxil --dxbc: %s 有 %zu 个循环必须展开（X3531），这些循环不加 [loop] 后重编\n",
                         input, added);
        }
        // 兜底：仍然报 X3531（报错位置对不上插入的 [loop]）时，整个着色器都不加 [loop] 再试一次
        if (!ok && diagnostics.find("X3531") != std::string::npos) {
            std::fprintf(stderr, "ShaderToDxil --dxbc: %s 仍报 X3531，改为整体不加 [loop] 重编\n", input);
            if (!buildPatched(false, regs)) return 1;
            ok = compile(regs.hlsl, code);
        }
    }
    // 失败时是错误，成功时是警告；都原样打印（每行就是「文件(行,列): 消息」）
    if (!diagnostics.empty()) std::fprintf(stderr, "%s", diagnostics.c_str());
    if (!ok) {
        std::fprintf(stderr, "ShaderToDxil --dxbc: fxc 编译失败（%s）\n", input);
        return 1;
    }

    // DXBC 容器之后附上绑定表（格式见 RHI/DxbcBindTable.h）
    const auto* dxbcBegin = static_cast<const unsigned char*>(code->GetBufferPointer());
    std::vector<unsigned char> bytes(dxbcBegin, dxbcBegin + code->GetBufferSize());
    code->Release();
    const size_t dxbcSize = bytes.size();
    const std::vector<unsigned char> table = SerializeBindTable(regs.bindings);
    bytes.insert(bytes.end(), table.begin(), table.end());

    if (!WriteFile(output, MakeHeader(bytes, symbol))) {
        std::fprintf(stderr, "ShaderToDxil: 无法写 %s\n", output);
        return 1;
    }
    std::fprintf(stderr, "    DXBC: %s (%zu 字节 + 绑定表 %zu 项) -> %s\n",
                 symbol, dxbcSize, regs.bindings.size(), output);
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
        // 不要把所有 cbuffer 归一到 b0（曾经的 PatchCBuffers，已删除）：多个 cbuffer 会重叠
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
