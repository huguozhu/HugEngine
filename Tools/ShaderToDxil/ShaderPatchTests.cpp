// ============================================================================
//  ShaderPatchTests —— ShaderToDxil 的 D3D11 补丁单元测试
//
//  手写 Slang 风格的 HLSL 片段（带 #line 指回临时写出的 .slang 源文件），
//  检查补丁后的文本与绑定表和期望完全一致。
//  运行：构建 ShaderToDxilTests 目标后直接执行，返回 0 表示全部通过。
// ============================================================================

#include "ShaderPatch.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace shaderpatch;
using he::rhi::dxbc::DxbcBindEntry;
using he::rhi::dxbc::DxbcBindType;

static int g_Failures = 0;

#define CHECK(cond) do { if (!(cond)) { ++g_Failures; std::fprintf(stderr, "  失败：%s（第 %d 行）\n", #cond, __LINE__); } } while (0)

/// 文本比较，失败时打印两边内容
static void CheckText(const char* what, const std::string& got, const std::string& want) {
    if (got == want) return;
    ++g_Failures;
    std::fprintf(stderr, "  失败：%s 文本不一致\n----- 实际 -----\n%s\n----- 期望 -----\n%s\n----------------\n",
                 what, got.c_str(), want.c_str());
}

static void ReplaceAll(std::string& s, const std::string& from, const std::string& to) {
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) s.replace(p, from.size(), to);
}

/// 把 Slang 源码写到临时目录，返回路径（正斜杠，与 Slang 输出的 #line 路径格式一致）
static std::string WriteSource(const char* name, const std::string& text) {
    const auto dir = std::filesystem::temp_directory_path() / "ShaderPatchTests";
    std::filesystem::create_directories(dir);
    const auto path = dir / name;
    std::ofstream(path, std::ios::binary) << text;
    std::string p = path.generic_string();
    return p;
}

static bool SameEntry(const DxbcBindEntry& e, uint32_t set, uint32_t binding, DxbcBindType type,
                      uint32_t slot, uint32_t count) {
    return e.set == set && e.binding == binding && e.type == static_cast<uint32_t>(type) &&
           e.slot == slot && e.count == count;
}

// ---------------------------------------------------------------------------
//  用例 1：像素着色器的寄存器分配
//   · t / u 用 binding 号，像素着色器的 UAV 平移到 u8 之后
//   · 采样器压缩到 s0.. （binding 22 超出 s0..s15，必须压缩）
//   · 描述符集 cbuffer 压缩到 b1..，推送常量固定 b0
//   · 源码显式写的 register(t1792, space2) 丢弃，按 vk::binding 重新分配
// ---------------------------------------------------------------------------
static void TestPixelRegisters() {
    std::fprintf(stderr, "用例 1：像素着色器寄存器分配\n");
    const std::string src = WriteSource("case1.slang",
        "[[vk::binding(3, 0)]] Texture2D<float4> u_Albedo;\n"
        "[[vk::binding(4, 0)]] SamplerState u_AlbedoSampler;\n"
        "[[vk::binding(22, 0)]] SamplerState u_PointSampler;\n"
        "[[vk::binding(5, 0)]] cbuffer LightCB { float4 c; };\n"
        "[[vk::push_constant]] cbuffer PC { float4 p; };\n"
        "[[vk::binding(2, 0)]] RWStructuredBuffer<uint> u_Counter;\n"
        "[[vk::binding(30, 0)]] StructuredBuffer<float4> u_Mats[4] : register(t1792, space2);\n");

    std::string hlsl =
        "#line 1 \"SRC\"\n"
        "Texture2D<float4 > u_Albedo_0 : register(t0);\n"
        "#line 2\n"
        "SamplerState u_AlbedoSampler_0 : register(s0);\n"
        "#line 3\n"
        "SamplerState u_PointSampler_0 : register(s1);\n"
        "#line 4\n"
        "cbuffer LightCB_0 : register(b0)\n"
        "{\n"
        "    float4 c_0;\n"
        "}\n"
        "#line 5\n"
        "cbuffer PC_0 : register(b1)\n"
        "{\n"
        "    float4 p_0;\n"
        "}\n"
        "#line 6\n"
        "RWStructuredBuffer<uint > u_Counter_0 : register(u0);\n"
        "#line 7\n"
        "StructuredBuffer<float4 >  u_Mats_0[int(4)] : register(t1792, space2);\n";
    ReplaceAll(hlsl, "SRC", src);

    std::string want =
        "#line 1 \"SRC\"\n"
        "Texture2D<float4 > u_Albedo_0 : register(t3);\n"
        "#line 2\n"
        "SamplerState u_AlbedoSampler_0 : register(s0);\n"
        "#line 3\n"
        "SamplerState u_PointSampler_0 : register(s1);\n"
        "#line 4\n"
        "cbuffer LightCB_0 : register(b1)\n"
        "{\n"
        "    float4 c_0;\n"
        "}\n"
        "#line 5\n"
        "cbuffer PC_0 : register(b0)\n"
        "{\n"
        "    float4 p_0;\n"
        "}\n"
        "#line 6\n"
        "RWStructuredBuffer<uint > u_Counter_0 : register(u10);\n"
        "#line 7\n"
        "StructuredBuffer<float4 >  u_Mats_0[int(4)] : register(t30);\n";
    ReplaceAll(want, "SRC", src);

    const D3D11RegisterResult r = PatchRegistersD3D11(hlsl, /*isPixelStage=*/true);
    CheckText("用例 1 HLSL", r.hlsl, want);
    CHECK(r.errors.empty());
    for (const auto& e : r.errors) std::fprintf(stderr, "    意外错误：%s\n", e.c_str());
    CHECK(r.bindings.size() == 7);
    if (r.bindings.size() == 7) {
        CHECK(SameEntry(r.bindings[0], 0, 3,  DxbcBindType::SRV,          3,  1));
        CHECK(SameEntry(r.bindings[1], 0, 4,  DxbcBindType::Sampler,      0,  1));
        CHECK(SameEntry(r.bindings[2], 0, 22, DxbcBindType::Sampler,      1,  1));
        CHECK(SameEntry(r.bindings[3], 0, 5,  DxbcBindType::CBuffer,      1,  1));
        CHECK(SameEntry(r.bindings[4], 0, 0,  DxbcBindType::PushConstant, 0,  1));
        CHECK(SameEntry(r.bindings[5], 0, 2,  DxbcBindType::UAV,          10, 1));
        CHECK(SameEntry(r.bindings[6], 0, 30, DxbcBindType::SRV,          30, 4));
    }

    // 同一份 HLSL 当作计算着色器：UAV 不平移
    const D3D11RegisterResult cs = PatchRegistersD3D11(hlsl, /*isPixelStage=*/false);
    CHECK(cs.hlsl.find("u_Counter_0 : register(u2);") != std::string::npos);
}

// ---------------------------------------------------------------------------
//  用例 2：无法用于 D3D11 的声明必须报错（不静默）
//   · 无界数组；采样器超过 16 个；两个 SRV 区间重叠
// ---------------------------------------------------------------------------
static void TestRegisterErrors() {
    std::fprintf(stderr, "用例 2：寄存器错误检测\n");
    const std::string src = WriteSource("case2.slang",
        "[[vk::binding(0, 0)]] Texture2D<float4> u_All[];\n"
        "[[vk::binding(1, 0)]] SamplerState u_Many[17];\n"
        "[[vk::binding(4, 0)]] Texture2D<float4> u_Range[4];\n"
        "[[vk::binding(6, 0)]] Texture2D<float4> u_Clash;\n");

    std::string hlsl =
        "#line 1 \"SRC\"\n"
        "Texture2D<float4 >  u_All_0[] : register(t0, space1);\n"
        "#line 2\n"
        "SamplerState  u_Many_0[int(17)] : register(s0);\n"
        "#line 3\n"
        "Texture2D<float4 >  u_Range_0[int(4)] : register(t1);\n"
        "#line 4\n"
        "Texture2D<float4 > u_Clash_0 : register(t5);\n";
    ReplaceAll(hlsl, "SRC", src);

    const D3D11RegisterResult r = PatchRegistersD3D11(hlsl, false);
    CHECK(r.errors.size() == 3);
    auto has = [&](const char* var, const char* key) {
        for (const auto& e : r.errors) {
            if (e.rfind(var, 0) == 0 && e.find(key) != std::string::npos) return true;
        }
        return false;
    };
    CHECK(has("u_All",   "无界"));
    CHECK(has("u_Many",  "采样器超过 16 个"));
    CHECK(has("u_Clash", "重叠"));
    for (const auto& e : r.errors) std::fprintf(stderr, "    （预期内的错误）%s\n", e.c_str());
}

// ---------------------------------------------------------------------------
//  用例 3：fxc 的循环属性
//   · [unroll] 全部去掉；只有最外层循环加 [loop]；do-while 的尾部 while 不算新循环
// ---------------------------------------------------------------------------
static void TestLoops() {
    std::fprintf(stderr, "用例 3：循环属性\n");
    std::string hlsl =
        "float f(int n)\n"
        "{\n"
        "    float s = 0;\n"
        "    // for(;;) 写在注释里不算\n"
        "    [unroll]\n"
        "    for(;;)\n"
        "    {\n"
        "        if (s > 1) { break; }\n"
        "        for(;;)\n"
        "        {\n"
        "            break;\n"
        "        }\n"
        "    }\n"
        "    do\n"
        "    {\n"
        "        s += 1;\n"
        "    } while(s < 4);\n"
        "    [loop] for(;;) { break; }\n"
        "    return s;\n"
        "}\n";
    const std::string want =
        "float f(int n)\n"
        "{\n"
        "    float s = 0;\n"
        "    // for(;;) 写在注释里不算\n"
        "    \n"
        "    [loop] for(;;)\n"
        "    {\n"
        "        if (s > 1) { break; }\n"
        "        for(;;)\n"
        "        {\n"
        "            break;\n"
        "        }\n"
        "    }\n"
        "    [loop] do\n"
        "    {\n"
        "        s += 1;\n"
        "    } while(s < 4);\n"
        "    [loop] for(;;) { break; }\n"
        "    return s;\n"
        "}\n";
    PatchLoopsFxc(hlsl);
    CheckText("用例 3 HLSL", hlsl, want);
}

// ---------------------------------------------------------------------------
//  用例 4：绑定表附在 DXBC 容器之后，运行时能原样解析回来
// ---------------------------------------------------------------------------
static void TestBindTableRoundTrip() {
    std::fprintf(stderr, "用例 4：绑定表序列化与解析\n");
    // 伪造一个 32 字节的 DXBC 容器：魔数 "DXBC"，第 24 字节记录容器总长度
    std::vector<unsigned char> blob(32, 0);
    std::memcpy(blob.data(), "DXBC", 4);
    const uint32_t containerSize = 32;
    std::memcpy(blob.data() + 24, &containerSize, 4);

    std::vector<DxbcBindEntry> in(2);
    in[0] = { 0, 3, static_cast<uint32_t>(DxbcBindType::SRV), 3, 1 };
    in[1] = { 0, 22, static_cast<uint32_t>(DxbcBindType::Sampler), 1, 1 };
    const auto table = SerializeBindTable(in);
    blob.insert(blob.end(), table.begin(), table.end());

    CHECK(he::rhi::dxbc::GetDxbcContainerSize(blob.data(), blob.size()) == 32);
    const DxbcBindEntry* entries = nullptr;
    const auto* header = he::rhi::dxbc::FindDxbcBindTable(blob.data(), blob.size(), &entries);
    CHECK(header != nullptr);
    if (header) {
        CHECK(header->entryCount == 2);
        CHECK(SameEntry(entries[0], 0, 3,  DxbcBindType::SRV,     3, 1));
        CHECK(SameEntry(entries[1], 0, 22, DxbcBindType::Sampler, 1, 1));
    }
    // 截断的数据不能被当成合法的表
    CHECK(he::rhi::dxbc::FindDxbcBindTable(blob.data(), blob.size() - 4, &entries) == nullptr);
}

// ---------------------------------------------------------------------------
//  用例 5：结构化缓冲里的矩阵在读取处补 transpose（fxc 忽略 column_major）
// ---------------------------------------------------------------------------
static void TestStructuredMatrices() {
    std::fprintf(stderr, "用例 5：结构化缓冲矩阵修正\n");
    std::string hlsl =
        "struct Inner_0\n"
        "{\n"
        "    float4x4  m_0[int(2)];\n"
        "};\n"
        "struct Obj_0\n"
        "{\n"
        "    float4x4 world_0;\n"
        "    Inner_0 inner_0;\n"
        "    float4 color_0;\n"
        "};\n"
        "struct Plain_0\n"
        "{\n"
        "    float4 a_0;\n"
        "};\n"
        "StructuredBuffer<Obj_0 > u_Objs_0 : register(t2);\n"
        "StructuredBuffer<Plain_0 > u_Plain_0 : register(t3);\n"
        "StructuredBuffer<uint > u_Idx_0 : register(t4);\n"
        "float4 f(uint i) { return mul(u_Objs_0.Load(u_Idx_0.Load(i)).world_0, u_Plain_0.Load(i).a_0) + u_Objs_0[i].color_0; }\n";
    const std::string want =
        "struct Inner_0\n"
        "{\n"
        "    float4x4  m_0[int(2)];\n"
        "}; Inner_0 HE_FxcMat_Inner_0(Inner_0 v) { v.m_0[0] = transpose(v.m_0[0]); v.m_0[1] = transpose(v.m_0[1]); return v; }\n"
        "struct Obj_0\n"
        "{\n"
        "    float4x4 world_0;\n"
        "    Inner_0 inner_0;\n"
        "    float4 color_0;\n"
        "}; Obj_0 HE_FxcMat_Obj_0(Obj_0 v) { v.world_0 = transpose(v.world_0); v.inner_0 = HE_FxcMat_Inner_0(v.inner_0); return v; }\n"
        "struct Plain_0\n"
        "{\n"
        "    float4 a_0;\n"
        "};\n"
        "StructuredBuffer<Obj_0 > u_Objs_0 : register(t2);\n"
        "StructuredBuffer<Plain_0 > u_Plain_0 : register(t3);\n"
        "StructuredBuffer<uint > u_Idx_0 : register(t4);\n"
        "float4 f(uint i) { return mul(HE_FxcMat_Obj_0(u_Objs_0.Load(u_Idx_0.Load(i))).world_0, u_Plain_0.Load(i).a_0) + HE_FxcMat_Obj_0(u_Objs_0[i]).color_0; }\n";
    const auto problems = PatchStructuredMatricesFxc(hlsl);
    CheckText("用例 5 HLSL", hlsl, want);
    if (!problems.empty()) {
        std::fprintf(stderr, "  用例 5：不应有无法修正的情况，实际 %zu 条\n", problems.size());
        ++g_Failures;
    }
}

// ---------------------------------------------------------------------------
//  用例 6：if 语句加 [branch]（fxc 的 flattened side effect 重试用）
// ---------------------------------------------------------------------------
static void TestBranches() {
    std::fprintf(stderr, "用例 6：分支属性\n");
    std::string hlsl =
        "#if defined(X)\n"
        "void f(uint i)\n"
        "{\n"
        "    if(i > 0u) { u_Out[i] = 1u; }\n"
        "    else if (i == 0u) { u_Out[0] = 2u; }\n"
        "    [flatten] if (i > 5u) { }\n"
        "    uint gif = elif(i);\n"
        "}\n"
        "#endif\n";
    const std::string want =
        "#if defined(X)\n"
        "void f(uint i)\n"
        "{\n"
        "    [branch] if(i > 0u) { u_Out[i] = 1u; }\n"
        "    else if (i == 0u) { u_Out[0] = 2u; }\n"
        "    [flatten] if (i > 5u) { }\n"
        "    uint gif = elif(i);\n"
        "}\n"
        "#endif\n";
    PatchBranchesFxc(hlsl);
    CheckText("用例 6 HLSL", hlsl, want);
}

int main() {
    TestPixelRegisters();
    TestRegisterErrors();
    TestLoops();
    TestBindTableRoundTrip();
    TestStructuredMatrices();
    TestBranches();
    if (g_Failures == 0) {
        std::fprintf(stderr, "ShaderPatchTests：全部通过\n");
        return 0;
    }
    std::fprintf(stderr, "ShaderPatchTests：%d 处失败\n", g_Failures);
    return 1;
}
