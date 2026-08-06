# HugEngine

现代实时渲染引擎 — 对标 UE5，覆盖从 RHI 到神经网络渲染的完整技术栈。

## 目录结构

```
HugEngine/
├── Engine/         # 引擎全部源代码（含 External/ 第三方依赖）
├── Samples/        # 示例项目（01.Triangle ~ 04.Deferred + HugEditor）
├── Docs/           # 设计文档、架构规划
└── README.md
```

## 构建

```bash
# 初始化第三方依赖 (git submodule) + 配置 (需要 CMake 3.28+ / Vulkan SDK)
git submodule update --init --recursive
cmake -B build -S . --preset=default
cmake --build build
```

## 文档

- [技术全景与实施计划](Docs/HugEngine技术全景与实施计划.md)
- [架构设计与任务划分](Docs/HugEngine架构设计与任务划分.md)
- [开发进度](Docs/HugEngine开发进度.md)

## 技术栈

| 维度 | 选型 |
|------|------|
| 构建 | CMake + git submodule（ImGui 由 CDN 下载） |
| 语言 | C++20 |
| 着色器 | Slang → SPIR-V / DXIL |
| RHI | Vulkan 1.3+ / D3D12 SM 6.6+ |
| 数学 | GLM |
| 编辑器 | Dear ImGui |
| 反射 | 宏驱动运行时反射（HE_CLASS 宏，预留 C++26 ^T 后端） |
