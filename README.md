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

- [01 技术全景与实施计划](docs/HugEngine引擎介绍/01.技术全景与实施计划.md)（技术清单 / 分阶段路线图）
- [02 架构 UML 与可扩展性分析](docs/HugEngine引擎介绍/02.架构UML与可扩展性分析.md)（架构总览 / 分层类图 / 关键时序 / 可扩展性与债务）
- [03 技术名词全目录](docs/HugEngine引擎介绍/03.技术名词全目录.md)（117 条术语索引，当字典用）
- [04 功能缺口与对标分析](docs/HugEngine引擎介绍/04.功能缺口与对标分析.md)（RHI / 渲染 / 游戏层 / 工程化缺口 + 16 条代码级债务）
- [05 渲染管线实现分析](docs/HugEngine引擎介绍/05.渲染管线实现分析.md)（Forward+ / Deferred / PathTracing 三管线 + 公共基础设施）
- [06 材质系统实现分析](docs/HugEngine引擎介绍/06.材质系统实现分析.md)（GBuffer 通道 / PBR 参数 / 数学原理）
- [07 全局光照（GI）本质、实现与架构优化](docs/HugEngine引擎介绍/07.全局光照GI本质、实现与架构优化.md)（IBL / SSGI / SSR / RSM / DDGI / 层栈合成）
- [08 Lumen 实现分析](docs/HugEngine引擎介绍/08.Lumen实现分析.md)（Mesh/Global SDF / Surface Cache / Screen Probe / 远场 HW RT）
- [09 硬件光追与路径追踪实现分析](docs/HugEngine引擎介绍/09.硬件光追与路径追踪实现分析.md)（RTEffectPass 四效果 / PathTracingPipeline / ReSTIR / STBN）
- [10 Nanite 实现分析](docs/HugEngine引擎介绍/10.Nanite实现分析.md)（数学原理 / 架构设计 / 实现细节）
- [11 后处理链与抗锯齿实现分析](docs/HugEngine引擎介绍/11.后处理链与抗锯齿实现分析.md)（ToneMap/Bloom/DOF/降噪体系 / TAA·FXAA·SMAA·MSAA 实情）
- [12 阴影系统实现分析](docs/HugEngine引擎介绍/12.阴影系统实现分析.md)（双层策略模式 / CSM / 点光 / 聚光 / 矩形光 / 与 RT 阴影切换）
- [13 Shader 系统实现分析](docs/HugEngine引擎介绍/13.Shader系统实现分析.md)（slangc 构建管线 / 15 组着色器目录 / 绑定约定 / 热重载边界）
- [14 RHI 架构与 Vulkan 实现分析](docs/HugEngine引擎介绍/14.RHI架构与Vulkan实现分析.md)（抽象接口层 / Vulkan 后端 / 延迟销毁与 PSO 管线）
- [15 多线程架构与渲染实现分析](docs/HugEngine引擎介绍/15.多线程架构与渲染实现分析.md)（JobSystem / 并行剔除 / MTCR / AsyncCompute）
- [16 Entity-Component 架构与组件功能](docs/HugEngine引擎介绍/16.Entity-Component%20架构与组件功能.md)（组件总表 + 逐组件详解 + 扩展指南）
- [17 Physics 系统实现分析](docs/HugEngine引擎介绍/17.Physics系统实现分析.md)（Jolt 封装 / 固定步长 tick / 查询与角色移动）
- [18 资产管线与 glTF 加载实现分析](docs/HugEngine引擎介绍/18.资产管线与glTF加载实现分析.md)（加载器 / 顶点与坐标系约定 / 骨骼与动画）
- [19 反射与序列化实现分析](docs/HugEngine引擎介绍/19.反射与序列化实现分析.md)（宏驱动注册 / TypeRegistry / 属性注解 / `.hescene` 存取）
- [20 编辑器实现分析](docs/HugEngine引擎介绍/20.编辑器实现分析.md)（ImGui 集成 / 10 个面板 / Gizmo / 撤销体系）
- [21 构建与依赖管理](docs/HugEngine引擎介绍/21.构建与依赖管理.md)（preset / 11 个第三方库 / 新增依赖步骤）
- [22 开发与验证手册](docs/HugEngine引擎介绍/22.开发与验证手册.md)（构建 / 9 个示例导读 / 测试 / 39 个验证脚本 / 环境变量与 CVar）
- [23 AI 架构设计](docs/HugEngine引擎介绍/23.AI架构设计.md)（AI 一等公民 / 统一基座 / AIGC 平台 / LLM 协议与实现计划）

> 该目录是 **HugEngine 自身的技术文档**（共 23 篇）；**文件名序号即推荐阅读顺序**：
> `01–04` 认识工程（全景 → 架构 → 名词 → 缺口）→ `05–13` 渲染主线（管线 → 材质 → GI → Lumen → 光追/PT → Nanite → 后处理 → 阴影 → Shader）→
> `14–15` 平台层（RHI → 多线程）→ `16–20` 场景与游戏层（ECS → 物理 → 资产 → 反射序列化 → 编辑器）→ `21–22` 上手与工程化 → `23` AI。
>
> **重命名对照（2026-09-22 按阅读顺序编号）**：全部文档加了 `NN.` 前缀并去掉 `HugEngine` 前缀，旧的文档链接会失效，对照：`01`←技术全景与实施计划 · `02`←架构UML与可扩展性分析 · `03`←技术名词全目录 · `04`←功能缺口与对标分析 · `05`←渲染管线实现分析 · `06`←材质系统实现分析 · `07`←全局光照GI本质、实现与架构优化 · `08`←Lumen实现分析 · `09`←RayTracing与路径追踪实现分析 · `10`←Nanite实现分析 · `11`←后处理与抗锯齿实现分析 · `12`←阴影系统实现分析 · `13`←Shader系统实现分析 · `14`←RHI架构与Vulkan实现分析 · `15`←多线程架构与渲染实现分析 · `16`←Entity-Component 架构与组件功能 · `17`←Physics系统实现分析 · `18`←资产管线与glTF加载实现分析 · `19`←反射与序列化实现分析 · `20`←编辑器实现分析 · `21`←构建与依赖管理 · `22`←开发与验证手册 · `23`←AI架构设计/（以上旧名均带 `HugEngine` 前缀）。

**专项文档**

- [已实现功能](docs/已实现功能/)（RHI / 渲染管线 / GI / **Nanite 虚拟几何** / 物理 / 编辑器等落地设计与判据）
- [计划实现功能](docs/计划实现功能/)（**已规划未开工**：[渲染线程化实施方案](docs/计划实现功能/HugEngine渲染线程化实施方案.md)——Game Thread → Render+RHI 单线程）
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
