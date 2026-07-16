---
name: gpu-particle-system
description: GPU 粒子系统完整实现 — 发射/模拟/剔除/Billboard 渲染，参数化颜色/大小
metadata:
  node_type: memory
  type: project
---

# GPU 粒子系统

## 状态
✅ 已完成 — 粒子发射、物理模拟、视锥剔除、Billboard 渲染均已验证通过。

## 架构
```
DispatchCompute (每帧):
  Init (仅一次) → Emit → Simulate (遍历全部粒子) → Culling
Render (FrameGraph):
  ParticleRender pass → Draw(6, renderCount)
```

## 功能清单

| 功能 | 状态 |
|------|------|
| GPU 粒子发射（球体/盒子形状、多种方向模式） | ✅ |
| 每帧全粒子物理模拟（速度+重力+Euler 积分） | ✅ |
| 视锥剔除 | ✅ |
| BillBoard 渲染（View Space 构建，始终面向相机） | ✅ |
| 参数化粒子大小（minSize ~ maxSize 随机） | ✅ |
| 参数化粒子颜色（startColor ~ endColor 按生命插值） | ✅ |
| 粒子生命周期管理（死亡回收 → DeadList） | ✅ |
| 每粒子唯一种子（全局原子计数器） | ✅ |
| 多彩粒子（HSV 基于 particleIdx） | ✅ |
| DebugDumpState：GPU 数据回读诊断 | ✅ |

## 待实现

| 功能 | 优先级 |
|------|--------|
| 深度排序（Bitonic Sort） | P4 |
| 软粒子（GBuffer 深度混合） | P5 |
| 纹理序列帧动画 | P3 |
| ColorOverLife / SizeOverLife 渐变纹理 | P3 |
| 风力/外力场 | P5 |
| Mesh 粒子 | P7 |

## 关键踩坑

### 1. LoadOp 导致黑屏
- **现象**: 粒子 RenderPass `loadOp=CLEAR` 清除了 HDR Target
- **修复**: PSO 设置 `LoadOp::Load`

### 2. Push Constant 布局不匹配
- **现象**: GPU shader 不写入 counters，`renderCount` 永远为 0
- **根因**: C++ `glm::vec3` alignas(16) vs SPIR-V std140 布局
- **修复**: 重写 push constant 使用原始 `float[3]` + 显式 `_pad`，用 `spirv-cross --reflect` 验证

### 3. VulkanBuffer Map/Unmap 空操作
- `Map()` 需要 `vkInvalidateMappedMemoryRanges`（GPU→CPU）
- `Unmap()` 需要 `vkFlushMappedMemoryRanges`（CPU→GPU）

### 4. renderCount CPU 清零导致渲染跳过
- **现象**: GPU 计算了 renderCount，但 Render 读到 CPU 清零后的 0
- **根因**: DispatchCompute 中 CPU 写 `renderCount=0`，同帧 Render 读时 GPU 尚未执行完
- **修复**: DispatchCompute 开始时缓存上帧 GPU 的 renderCount，Render 使用缓存值

### 5. 粒子全部同位置/同色
- **现象**: 所有粒子位置、方向、颜色完全相同
- **根因**: 每帧 emitCount=1，`tid.x` 始终为 0，seed 不变
- **修复**: 全局原子计数器生成每粒子唯一种子

### 6. 粒子不运动
- **现象**: 粒子发射后静止不动
- **根因**: Simulate shader 仅处理 alivePre 中的粒子（= 本帧新发射），已存活粒子不更新
- **修复**: Simulate shader 改为遍历所有粒子池槽位，跳过未初始化/已死亡的

### 7. Billboard 不面向相机
- **根因**: 旧方案手动从 View 矩阵提取相机轴在世界空间构建，矩阵约定不匹配
- **修复**: 改为 View Space 中 XY 平面偏移角顶点 → 投影

## 文件清单

### 核心
- `Engine/Scene/Scene/ParticleComponent.h` — GPU 结构体定义 + ParticleSystemParam
- `Engine/Scene/Scene/ParticleComponent.cpp` — CPU 端生命周期管理
- `Engine/Render/Pipeline/ParticleRenderer.h/cpp` — 渲染器封装（PSO + Buffers + Dispatch）
- `Engine/Render/Pipeline/DeferredPipeline_FrameGraph.cpp` — FrameGraph 集成
- `Engine/Scene/Scene/SceneReflect.cpp` — 组件注册

### Shader
- `Engine/Shader/Shaders/Particles/ParticleTypes.slang` — GPU 共享类型定义
- `Engine/Shader/Shaders/Particles/ParticleInit.comp.slang` — 初始化 DeadList
- `Engine/Shader/Shaders/Particles/ParticleEmit.comp.slang` — 粒子发射
- `Engine/Shader/Shaders/Particles/ParticleSimulate.comp.slang` — 粒子物理模拟
- `Engine/Shader/Shaders/Particles/ParticleCulling.comp.slang` — 视锥剔除
- `Engine/Shader/Shaders/Particles/ParticleRender.vert.slang` — Billboard 顶点着色器
- `Engine/Shader/Shaders/Particles/ParticleRender.frag.slang` — 粒子片元着色器

### 测试
- `Samples/02.Cube/02.Cube.cpp` — 粒子系统集成测试
