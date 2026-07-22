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
