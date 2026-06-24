# HugEngine

现代实时渲染引擎 — 对标 UE5，覆盖从 RHI 到神经网络渲染的完整技术栈。

## 目录结构

```
HugEngine/
├── Engine/         # 引擎全部源代码
├── Samples/        # 示例项目
├── External/       # 第三方依赖 (vcpkg 管理)
├── Docs/           # 设计文档、架构规划
└── README.md
```

## 构建

```bash
# 配置 (需要 CMake 3.28+ 和 vcpkg)
cmake -B build -S . --preset=default
cmake --build build
```

## 文档

- [技术全景与实施计划](Docs/HugEngine_Technical_Plan.md)
- [架构设计与任务划分](Docs/HugEngine_Architecture_And_Tasks.md)

## 技术栈

| 维度 | 选型 |
|------|------|
| 构建 | CMake + vcpkg |
| 语言 | C++26 |
| 着色器 | Slang → SPIR-V / DXIL |
| RHI | Vulkan 1.3+ / D3D12 SM 6.6+ |
| 数学 | GLM |
| 编辑器 | Dear ImGui |
| 反射 | C++26 ^T + [[engine::]] 静态反射 |
