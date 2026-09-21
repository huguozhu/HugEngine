# HugEngine

现代实时渲染引擎 — 对标 UE5，覆盖从 RHI 到神经网络渲染的完整技术栈。

## 目录结构

```
HugEngine/
├── Engine/         # 引擎全部源代码（含 External/ 第三方依赖）
├── Samples/        # 示例项目（01.Triangle ~ 06.GILab + Editor 编辑器）
├── docs/           # 设计文档、架构规划
└── README.md
```

## 构建

```bash
# 初始化第三方依赖 (git submodule) + 配置 (需要 CMake 3.28+ / Vulkan SDK)
git submodule update --init --recursive
cmake --preset=default      # VS 2026 / Vulkan / 示例与编辑器 ON（见 CMakePresets.json）
cmake --build Build         # 需要 Release：cmake --build Build --config Release
```

## 示例

| 示例 | 内容 | 关注点 |
|---|---|---|
| `01.Triangle` | RHI 画三角形：光栅化 / Ray Tracing / Mesh Shader 三种模式 | RHI 后端、内嵌 SPIR-V、ImGui 切换 |
| `02.Cube` | PBR 前向渲染 + 全组件演示场 | 相机 / 天空 / 贴花 / 文字 / 粒子 / 碰撞 / 移动 / 万级实例 / 骨骼动画 / 样条（组件能力的综合载体） |
| `03.Sponza-Forward` | glTF 加载 Sponza 场景 + 自由相机漫游 | glTFLoader、PBR 纹理、RT 着色器 |
| `04.Sponza-Deferred` | 同一 Sponza 场景改走延迟管线 | GBuffer + 全屏 Lighting Pass |
| `05.Sponza-PathTracing` | 同一 Sponza 场景改走全路径追踪管线 | PT 参考渲染器（NEE + MIS + 俄罗斯轮盘赌 → ReSTIR DI → 时域降噪 → A-Trous）、RT 加速结构、r.PT.* 质量参数 |
| `07.AISamples` | AI 综合示例（TabBar 切 6 个模块，场景相互隔离） | LLM 场景生成 / 智能体 / GPU 推理 / 文生纹理 / 文生材质 / 文生网格动画 |
| `06.GILab` | Cornell Box + GI 对比与开发实验室 | IBL / RSM / SSGI / DDGI / SSR / RTGI 同场景对比、白炉探针、纹理落盘 |
| `Editor` | 编辑器应用 | Outliner / Details / Viewport / Content Browser / Stats / Console 面板、场景序列化、命令撤销、拖放导入 |

## 文档

**引擎介绍**（`docs/HugEngine引擎介绍/`）

- [Entity-Component 架构与组件功能](docs/HugEngine引擎介绍/HugEngine Entity-Component 架构与组件功能.md)
- [架构 UML 文档](docs/HugEngine引擎介绍/HugEngine架构UML文档.md)
- [技术全景与实施计划](docs/HugEngine引擎介绍/HugEngine技术全景与实施计划.md)
- [技术名词全目录](docs/HugEngine引擎介绍/HugEngine技术名词全目录.md)
- [渲染管线实现分析](docs/HugEngine引擎介绍/HugEngine渲染管线实现分析.md) / [RHI 架构与 Vulkan 实现分析](docs/HugEngine引擎介绍/HugEngine RHI架构与Vulkan实现分析.md)
- [全局光照（GI）本质、实现与架构优化](docs/HugEngine引擎介绍/HugEngine全局光照GI本质、实现与架构优化.md)
- [AI 架构设计](docs/HugEngine引擎介绍/HugEngine AI架构设计/)（AI 一等公民 / 统一基座 / AIGC 平台 / LLM 通信协议，共 9 篇）

> 该目录是 **HugEngine 自身的技术文档**（共 12 篇 + AI 架构设计 9 篇）：架构总览、各子系统实现分析（渲染管线 / RHI / GI / 多线程 / 可扩展性）、规划评审与缺口分析。

**架构与规划**

- [架构设计与任务划分](docs/HugEngine架构设计与任务划分.md)
- [开发进度](docs/HugEngine开发进度.md)

**专项文档**

- [已实现功能](docs/已实现功能/)（RHI / 渲染管线 / GI / **Nanite 虚拟几何** / 物理 / 编辑器等落地设计与判据）
- [技术分析文档](docs/技术分析文档/)（**通用图形技术资料**：Vulkan/D3D11/D3D12/Metal 名称对照、Vulkan 渲染流程、物理相机与光源原理与数学）
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
