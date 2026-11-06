// ============================================================
// ShaderTypes/GPUDefines.slang — C++/Slang 共享契约的宏层
//
// 【定位】本文件只定义宏、不声明任何类型，是 ShaderTypes/ 下每个切片的共同前置。
//
// 【为什么没有 include guard】它要被 N 个切片各包含一次，必须允许重复展开：宏本身用
// `#ifndef GPU_UINT` 守卫，重复包含不会重定义。真正的 include guard 在每个**切片**上。
//
// 【为什么切片末尾要 #undef】C++ 侧允许直接 include 单个切片（这正是"减少展开"的手段），
// 此时 GPU_STRUCT（= struct alignas(16)）不应泄漏进包含者的后续代码，故每个切片的
// `#ifdef __cplusplus` 分支在末尾统一清理这 6 个宏；Slang 分支不清理，由总入口
// ShaderTypes.slang 末尾统一 #undef（与拆分成切片之前的行为一致）。
//
// 【何时用 GPU_STRUCT、何时用普通 struct】
//   · 大小已是 16 的整数倍（128 / 48 / 32）⇒ 用 GPU_STRUCT：C++ 端加 alignas(16)，
//     在 SSBO/UBO 里与 Slang 的 std430 对齐一致
//   · 大小**不是** 16 的倍数（20 / 8）⇒ 必须用**普通 struct**：GPU_STRUCT 的 alignas(16)
//     会把它们撑到 32 / 16 字节，直接破坏与 Vulkan 的二进制契约
//   · 结构里**不许**出现非 16B 步长的数组（如 float[3]）：Slang 的 cbuffer/std140 里
//     数组元素步长固定 16 字节，而 C++ 侧 float[3] 只有 12 字节 ⇒ 数组**之后**的所有
//     成员两端偏移都会错开。实测事故见 GIBlendParams 的 _padBlend（任务 34 / §9.2-AF）：
//     填充一律用 float4。
// ============================================================
#ifndef GPU_UINT
#ifdef __cplusplus
    // C++: Types.h 已提供 uint32_t / int32_t（通过 <cstdint>）
    #define GPU_UINT  uint32_t    // Slang 'uint' → C++ uint32_t
    #define GPU_INT   int32_t     // Slang 'int'  → C++ int32_t
    #define GPU_STRUCT struct alignas(16)  // Slang 自动按 GPU 规则对齐，C++ 需显式声明

    // GPU 常量（C++: static constexpr 内部链接，Slang: static const）
    #define GPU_CONST_BEGIN
    #define GPU_CONST_END
    #define GPU_CONST(decl) static constexpr decl
#else
    // Slang/HLSL: 原生 GPU 类型
    #define GPU_UINT  uint
    #define GPU_INT   int
    #define GPU_STRUCT struct       // Slang 不需要 alignas

    #define GPU_CONST_BEGIN
    #define GPU_CONST_END
    #define GPU_CONST(decl) static const decl
#endif
#endif // GPU_UINT
