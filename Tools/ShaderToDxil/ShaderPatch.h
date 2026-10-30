#pragma once

// ============================================================================
//  ShaderPatch —— ShaderToDxil 的 HLSL 文本补丁（从 main.cpp 拆出，便于单元测试）
//
//  输入都是 `slangc -target hlsl` 的输出文本（带 `#line` 指回 Slang 源文件）。
//  D3D12（DXIL）与 D3D11（DXBC）共用：语义注入、uint8_t 替换、实例号改写；
//  两者分开的是：循环属性处理、寄存器分配（D3D12 用 space，D3D11 没有 space）。
// ============================================================================

#include "RHI/DxbcBindTable.h"

#include <string>
#include <vector>

namespace shaderpatch {

// ---------------------------------------------------------------------------
//  D3D12 / D3D11 共用
// ---------------------------------------------------------------------------

/// 给 HLSL 里所有 Output/Input 结构体的无语义成员注入语义（VS 输出位置 → SV_Position，其余 TEXCOORDn）
std::string InjectSemantics(std::string src, bool isVertexStage);

/// uint8_t → uint：Slang 用 uint8_t 表达 8 位整数，HLSL 编译器不认识
void PatchUint8(std::string& s);

/// 顶点着色器的 SV_InstanceID 改为逐实例顶点属性 HE_INSTANCEINDEX（含起始实例，与 Vulkan 一致）
void PatchInstanceId(std::string& s);

// ---------------------------------------------------------------------------
//  D3D12（DXIL）专用
// ---------------------------------------------------------------------------

/// 给 `for (` 循环加 [loop]（D3D12 现有行为，保持不变）
void PatchLoops(std::string& s);

/// 按源码 [[vk::binding(B, S)]] 生成 register(<t|u|s>B, spaceS)；描述符集 cbuffer 为 b(B+8)
std::string PatchRegisters(const std::string& src);

// ---------------------------------------------------------------------------
//  D3D11（DXBC，SM 5.0）专用
// ---------------------------------------------------------------------------

/// fxc 专用的循环处理：
///  · 去掉所有 [unroll]（fxc 对含 break 的循环强制展开会报 X3511）；
///  · 只给**最外层**循环加 [loop]（嵌套循环加 [loop] 会触发 X3531），
///    内层循环交给 fxc 自行决定展开与否。
/// 循环在 Slang 源码里的位置（按 HLSL 里的 #line 推算）
struct LoopSite {
    std::string file;   // #line 给出的源文件路径
    int         line = 0;
};
/// insertLoop=false：只去掉 [unroll]、不加 [loop]。
/// skip：这些源码位置（同文件、行号相差 ≤ 2）的循环不加 [loop] —— fxc 报 X3531 说明该循环
/// 必须展开（如分支内带隐式导数采样的定长循环），见 main.cpp 的逐循环重试
void PatchLoopsFxc(std::string& s, bool insertLoop = true, const std::vector<LoopSite>* skip = nullptr);

/// D3D11 寄存器分配的结果
struct D3D11RegisterResult {
    std::string                                   hlsl;      // 改写后的 HLSL
    std::vector<he::rhi::dxbc::DxbcBindEntry>     bindings;  // 绑定表（按声明顺序）
    std::vector<std::string>                      errors;    // 非空 ⇒ 该着色器无法用于 D3D11
};

/// 按源码 [[vk::binding(B, S)]] 重新分配 D3D11 寄存器（去掉 space）：
///  · SRV：tB；UAV：uB（像素着色器平移到 u(8+B)，避开渲染目标）；
///  · 采样器：按声明顺序压缩到 s0..s15；
///  · 描述符集 cbuffer：按声明顺序压缩到 b1..b13；[[vk::push_constant]] 块固定 b0；
///  · 源码里显式写的 register(..., spaceN) 一律丢弃，按 vk::binding 重新分配；
///  · 槽位越界或重叠时记入 errors（不静默）。
D3D11RegisterResult PatchRegistersD3D11(const std::string& src, bool isPixelStage);

/// 绑定表序列化为字节（DxbcBindTableHeader + 各项），用于附在 DXBC 容器之后
std::vector<unsigned char> SerializeBindTable(const std::vector<he::rhi::dxbc::DxbcBindEntry>& bindings);

} // namespace shaderpatch
