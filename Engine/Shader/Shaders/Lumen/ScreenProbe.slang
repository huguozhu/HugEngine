// ScreenProbe.slang — Screen Probe 的数据布局（C++ / Slang **共享**，步骤 20）
//
// 【为什么先定结构】探针是 L3 的"辐照度载体"：位置/法线来自 GBuffer（本步），
// 入射辐射度由半球追踪得到（步骤 21），最后投影成二阶 SH（步骤 23，4 系数 RGB = 12 floats）。
// 布局一次定好（含 SH 字段），后续步骤只填内容 —— 避免"每加一个字段就要改两侧镜像 + 再抓一次布局错位"。
//
// 【合并语义】屏幕按 16×16 像素为一个"单元"；相邻 2×2 单元若法线足够一致（平坦区）就合并成
// 一个探针（占 32×32 像素），`meta.x` 记录该探针合并了几个单元（1 或 4）。
#ifndef HUGENGINE_SCREEN_PROBE
#define HUGENGINE_SCREEN_PROBE

#ifdef __cplusplus
    // 【为什么用 glm:: 全限定名】本头文件在**全局作用域**被包含，而引擎的 `float4` 是 `namespace he`
    // 里的 using 别名 —— 在全局作用域看不到（实测 C3646: "position": 未知重写说明符）。
    #include <cstdint>
    #include <glm/glm.hpp>
    #define SP_UINT   uint32_t
    #define SP_FLOAT4 glm::vec4
    #define SP_UINT4  glm::uvec4
    #define SP_STRUCT struct alignas(16)
    #define SP_CONST(decl) static constexpr decl
#else
    #define SP_UINT   uint
    #define SP_FLOAT4 float4
    #define SP_UINT4  uint4
    #define SP_STRUCT struct
    #define SP_CONST(decl) static const decl
#endif

// 探针（96 B：6 × 16 B，std430 下与 Slang 一致）
SP_STRUCT ScreenProbe {
    SP_FLOAT4 position;   // xyz = 世界坐标, w = 有效性（1 = 有几何）
    SP_FLOAT4 normal;     // xyz = 世界法线（已解码）, w = 线性深度（调试用）
    SP_FLOAT4 uv;         // xy = 屏幕 uv（probe 中心）, zw = 所属 tile 坐标
    SP_FLOAT4 shR;        // 二阶 SH 的 R 通道（L1：4 系数；步骤 23 填）
    SP_FLOAT4 shG;        //                     G
    SP_FLOAT4 shB;        //                     B
    SP_UINT4  meta;       // x = 合并了几个 16×16 单元（1/4）, y = 单元索引, z = 法线偏差（定点）, w = 保留
};

SP_CONST(SP_UINT kScreenProbeCellSize = 16u);   // 一个"单元"的像素边长
SP_CONST(SP_UINT kScreenProbeMaxMerge = 4u);    // 最多合并 2×2 个单元（32×32 像素）

#endif // HUGENGINE_SCREEN_PROBE
