---
name: gpu-particle-system
description: GPU 粒子系统完整实现 — 发射/模拟/剔除/Billboard 渲染，参数化颜色/大小
metadata:
  node_type: memory
  type: project
---

# GPU 粒子系统

## 状态
✅ 已完成 P1-P6 — 2026-07-16 ~ 2026-07-17

## 架构
```
DispatchCompute (每帧):
  Init (仅一次) → Emit → Simulate (遍历全部粒子) → Culling → Sort
Render (FrameGraph):
  ParticleRender pass → Draw(6, renderCount)
```

## 功能清单

| # | 功能 | 状态 |
|:---:|------|:---:|
| P1 | GPU 粒子发射（球体/盒子、多种方向模式） | ✅ |
| P2 | Billboard 渲染（View Space 构建，始终面向相机） | ✅ |
| P3 | 参数化粒子大小/颜色（minSize~maxSize, startColor~endColor） | ✅ |
| P4 | Bitonic Sort 深度排序（远→近渲染顺序） | ✅ |
| P5 | 软粒子（读 GBuffer 深度，近几何体时淡出） | ✅ |
| P6 | 多彩粒子（HSV 基于 particleIdx + 生命亮度衰减） | ✅ |
| - | 每帧全粒子物理模拟（速度+重力+Euler 积分） | ✅ |
| - | 每粒子唯一种子（全局原子计数器） | ✅ |
| - | 粒子生命周期管理（死亡回收→DeadList） | ✅ |
| - | DebugDumpState：GPU 数据回读诊断 | ✅ |

## 待实现

| # | 功能 | 说明 |
|:---:|------|------|
| - | **DrawIndirect** | GPU 写入 indirect args，消除 renderCount 帧间不匹配闪烁 |
| - | **风力/外力场** | 噪声风力、漩涡、点力场替代单一重力 |
| - | **SizeOverLife 渐变** | 粒子大小随生命周期变化（1D 纹理或曲线） |
| - | **拖尾/Trail** | 粒子运动轨迹拖尾效果 |
| - | **子粒子爆发** | 粒子死亡时生成子粒子（爆炸/火花效果） |
| - | **纹理序列帧动画** | Sprite sheet 子帧切换（需 bindless 纹理支持） |
| - | **完善 Sort** | 多 block Bitonic Sort（支持 >512 粒子） |
| - | **UI 参数面板** | ImGui 实时调节粒子参数 |
| - | ~~P7 Mesh 粒子~~ | Mesh Shader instanced — 2026-07-17 尝试后因 Slang 编译器 bug（`mul()` 在 mesh shader 中失效）放弃 |

## 设计规范要点（自《GPU 粒子系统设计规范》并入，2026-09-19）

> 并入原则：只保留**与当前实现一致**的内容。原文里已不存在的部分（独立的 `ParticleTickBegin.comp`
> 已被撤掉、per-pass 伪代码）以当前实现为准，不再保留。

### 数据流与缓冲

- **CPU 端**：`ParticleComponent`（`ParticleSystemParam`）——duration / particlesPerSec / 生命与初始速度区间 /
  发射形状（Sphere|Box）与方向模式（Directional|BiDirectional|Uniform2D|Uniform3D）/ texRowsCols /
  texFramesPerSec / texTimeSampling / gravity / minSize~maxSize / startColor~endColor /
  SizeOverLife / ColorOverLife / bindless 粒子纹理句柄。
- **GPU 端缓冲**（`ParticleRenderer`）：`deadList`、`alivePre`、`alivePost`、`counters`（原子计数）、
  `randomFloats`、`sortIndices`、`drawIndirectArgs` / `emitIndirectArgs` / `simIndirectArgs`、`billboardVB`。
- **每帧顺序**：Init（仅一次）→ Emit → Simulate（遍历整个粒子池）→ Culling → Sort
  →（帧图）ParticleRender → 写入 HDR 目标。

### 关键 GPU 结构（`Engine/Shader/Shaders/Particles/ParticleTypes.slang`，与 C++ 共享）

| 结构 | 内容 |
|---|---|
| `Particle`（32 B） | `life_time`(当前/总/纹理时间) + `tex_index` + `velocity`(xyz + 阻尼) + `position`(xyz + 当前大小) |
| `ParticleCounters` | `dead_count` / `alive_count[2]`（pre/post sim）/ `emit_count` / `simulate_count` / `render_count` |
| `SortInfo`（8 B） | `particle_index` + `particle_depth` |
| 常量 | `PARTICLE_CS_X_SIZE=32`、`BITONIC_BLOCK_SIZE=512`、`TRANSPOSE_BLOCK_SIZE=16`、`RANDOM_FLOAT_NUM=512` |

### Pass 职责（当前实现）

| Pass | 职责 |
|---|---|
| `ParticleInit.comp` | DeadList[i]=i、计数器复位（仅一次） |
| `ParticleEmit.comp` | 从 DeadList 原子取槽 → 随机位置/方向/速度/生命 → 写粒子与 alivePre |
| `ParticleSimulate.comp` | 全池遍历：Euler 积分（速度 + 重力 + 阻尼）、生命递减、纹理帧推进；死亡粒子归还 DeadList |
| `ParticleCulling.comp` | 视锥 / 距离剔除 → 填 SortIndices + 写 DrawIndirectArgs |
| `ParticleSort.comp` | Bitonic（块 512）+ 转置（>512），按深度排序 |
| `ParticleRender.vert.slang` / `.frag.slang` | 6 顶点 Billboard（View Space 构建）→ 片元做软粒子（采样场景深度）+ 参数化颜色 |

### 与 SeekEngine 的差异（设计选型，仍成立）

| 方面 | SeekEngine | HugEngine |
|---|---|---|
| 排序 | PreSort + Bitonic + Transpose（3 kernel） | 合并为单个 BitonicSort |
| 软粒子 | 不支持 | 读场景深度，近几何体时淡出 |
| 描述符 | 手动 SetParam | bindless 纹理数组 |
| Mesh 粒子 | 不支持 | 曾尝试 MeshShader instanced（P7）→ 因 Slang 编译 bug 放弃（见踩坑 9） |

### 里程碑与验证标准（P1~P7）

| # | 内容 | 验证标准 | 结果 |
|:---:|---|---|:---:|
| P1 | ParticleComponent + Init/Emit/Simulate | GPU 计数器正确、Debug 输出可读 | ✅ |
| P2 | Billboard 渲染 | 屏幕可见运动粒子 | ✅ |
| P3 | 参数化颜色/大小 | 粒子有颜色/大小变化 | ✅（纹理序列帧仍待实现） |
| P4 | Bitonic 深度排序 | 半透明粒子排序正确 | ✅ |
| P5 | 软粒子（GBuffer 深度混合） | 与几何体边缘平滑过渡 | ✅ |
| P6 | DeferredPipeline 集成 | 火焰/烟雾/雨雪效果 | ✅ |
| P7 | Mesh 粒子（MeshShader instanced） | 碎片/弹壳 | ❌ 放弃 |

## 关键踩坑

### 1. LoadOp 导致黑屏
- **现象**: 粒子 RenderPass `loadOp=CLEAR` 清除了 HDR Target
- **修复**: PSO 设置 `LoadOp::Load`

### 2. Push Constant 布局不匹配
- **现象**: GPU shader 不写入 counters，`renderCount` 永远为 0
- **根因**: C++ `glm::vec3` alignas(16) vs SPIR-V std140 布局
- **修复**: 重写 push constant 使用原始 `float[3]` + 显式 `_pad`

### 3. VulkanBuffer Map/Unmap 空操作
- `Map()` 需要 `vkInvalidateMappedMemoryRanges`（GPU→CPU）
- `Unmap()` 需要 `vkFlushMappedMemoryRanges`（CPU→GPU）

### 4. renderCount CPU 清零导致渲染跳过
- **现象**: GPU 计算了 renderCount，但 Render 读到 CPU 清零后的 0
- **修复**: DispatchCompute 开始时缓存上帧 GPU 的 renderCount，Render 使用缓存值
- **残余问题**: 粒子数下降时 cachedRenderCount > 实际数，读到旧 SortIndices → 轻微闪烁

### 5. 粒子全部同位置/同色
- **现象**: 所有粒子位置、方向、颜色完全相同
- **根因**: 每帧 emitCount=1，`tid.x` 始终为 0，seed 不变
- **修复**: 全局原子计数器生成每粒子唯一种子

### 6. 粒子不运动
- **现象**: 粒子发射后静止不动
- **根因**: Simulate shader 仅处理 alivePre 中的粒子（= 本帧新发射），已存活粒子不更新
- **修复**: Simulate shader 改为遍历所有粒子池槽位

### 7. Billboard 不面向相机
- **修复**: 改为 View Space 中 XY 平面偏移角顶点 → 投影

### 8. Fragment Shader push constant 导致颜色数据写入失败
- **现象**: 片元着色器中声明 push constant 后 VUID-01795，粒子不可见
- **修复**: fragment shader 移除 push constant 依赖，用 HSV(particleIdx) 生成颜色

### 9. Mesh Shader 中 `mul(matrix, vec)` 不工作
- **现象**: Mesh shader 中 `mul(float4x4, float4)` 导致几何体消失；单列 `dot()` 正常
- **根因**: Slang → SPIR-V 编译器 bug（多轴写入 output position 时）
- **状态**: 已放弃 P7，待 Slang 编译器修复后重试

## 文件清单

### 核心
- `Engine/Scene/Scene/ParticleComponent.h` — GPU 结构体定义 + ParticleSystemParam
- `Engine/Scene/Scene/ParticleComponent.cpp` — CPU 端生命周期管理
- `Engine/Render/Pipeline/ParticleRenderer.h/cpp` — 渲染器封装（PSO + Buffers + Dispatch）
- `Engine/Render/Pipeline/DeferredPipeline_FrameGraph.cpp` — FrameGraph 集成
- `Engine/Scene/Scene/SceneReflect.cpp` — 组件注册
- `Engine/RHI/RHI/CommandList.h` — 添加 DrawIndirect 接口
- `Engine/RHI/Vulkan/VulkanCommandList.h/cpp` — Vulkan DrawIndirect 实现
- `Engine/RHI/RHI/Types.h` — 添加 MeshShader PipelineStage

### Shader
- `Engine/Shader/Shaders/Particles/ParticleTypes.slang` — GPU 共享类型定义
- `Engine/Shader/Shaders/Particles/ParticleInit.comp.slang` — 初始化 DeadList
- `Engine/Shader/Shaders/Particles/ParticleEmit.comp.slang` — 粒子发射
- `Engine/Shader/Shaders/Particles/ParticleSimulate.comp.slang` — 粒子物理模拟
- `Engine/Shader/Shaders/Particles/ParticleCulling.comp.slang` — 视锥剔除
- `Engine/Shader/Shaders/Particles/ParticleSort.comp.slang` — Bitonic Sort 深度排序
- `Engine/Shader/Shaders/Particles/ParticleRender.vert.slang` — Billboard 顶点着色器
- `Engine/Shader/Shaders/Particles/ParticleRender.frag.slang` — 粒子片元着色器（软粒子+HSV）

### 测试
- `Samples/02.Cube/02.Cube.cpp` — 粒子系统集成测试（点发射、多彩色、慢速）
