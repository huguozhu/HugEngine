# HugEngine

现代实时渲染引擎 — 对标 UE5，覆盖从 RHI 到神经网络渲染的完整技术栈。

## 目录结构

```
HugEngine/
├── Engine/         # 引擎全部源代码（含 External/ 第三方依赖）
├── Samples/        # 示例项目（01.Triangle ~ 04.Deferred + HugEditor）
├── docs/           # 设计文档、架构规划
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

**引擎介绍**（`docs/HugEngine引擎介绍/`）

- [Entity-Component 架构与组件功能](docs/HugEngine引擎介绍/HugEngine Entity-Component 架构与组件功能.md)
- [架构 UML 文档](docs/HugEngine引擎介绍/HugEngine架构UML文档.md)
- [技术全景与实施计划](docs/HugEngine引擎介绍/HugEngine技术全景与实施计划.md)
- [技术名词全目录](docs/HugEngine引擎介绍/HugEngine技术名词全目录.md)
- [AI 架构设计](docs/HugEngine引擎介绍/HugEngine AI架构设计/)（AI 一等公民 / 统一基座 / AIGC 平台 / LLM 通信协议，共 9 篇）

**架构与规划**

- [架构设计与任务划分](docs/HugEngine架构设计与任务划分.md)
- [开发进度](docs/HugEngine开发进度.md)

**专项文档**

- [已实现功能](docs/已实现功能/)（RHI / 渲染管线 / GI / 物理 / 编辑器等落地设计与判据）
- [技术分析文档](docs/技术分析文档/)（架构分析、管线实现分析、GI 原理与现状、多线程渲染）
- [计划实现功能](docs/计划实现功能/)（Lumen/Nanite、全路径追踪、RHI 光追与 Mesh Shader 规划）
- [面试题集](docs/面试题集/)（C++ 多线程 30 题 / 高级图形开发 30 题）

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
