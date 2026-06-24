# Shader (L3)

着色器编译管线 — 基于 Slang 的多后端着色器系统。

| 子系统 | 说明 | Phase |
|--------|------|-------|
| Compiler | Slang → SPIR-V / DXIL 编译，Include 解析，预编译缓存 | P1 |
| HotReload | 文件监控 + 运行时重编译 + Pipeline 替换，不中断渲染 | P1 |

**依赖**: RHI
