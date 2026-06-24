# Core (L0)

平台抽象层 — 引擎与操作系统的唯一接口。所有平台相关代码集中在此。

| 子系统 | 说明 | Phase |
|--------|------|-------|
| Platform | 窗口 (GLFW/SDL3)、输入、文件 I/O | P1 |
| Math | GLM 封装 + SIMD 优化 (SSE/AVX/NEON) + 几何图元 | P1 |
| Memory | 分配器抽象、VMA/D3D12MA 封装、环形缓冲 | P1 |
| Containers | TArray, TMap, TSet, String, Span, Ref<T> | P1 |
| Threading | Taskflow Job System、Fiber、原子操作 | P1 |

**依赖**: 无（仅标准库和第三方库）
