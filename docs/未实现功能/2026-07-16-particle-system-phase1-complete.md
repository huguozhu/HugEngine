---
name: particle-system-phase1
description: GPU 粒子系统 Phase 1 — 基础管线打通（Emit/Sim/Cull/Render），已验证通过
metadata: 
  node_type: memory
  type: project
  originSessionId: 9aad420f-a838-48ff-99a3-be96da6dc2ff
---

# GPU 粒子系统 Phase 1 — 基础管线

## 状态
✅ 已完成 — 粒子发射、模拟、视锥剔除、Billboard 渲染均已验证通过。

## 架构
```
DispatchCompute (每帧):
  Init (仅一次) → Emit → Simulate → Culling
Render (FrameGraph):
  ParticleRender pass → Draw(6, renderCount)
```

## 关键踩坑

### 1. LoadOp 导致黑屏
- **现象**: 粒子 RenderPass 的 `loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR` 清除了 HDR Target
- **修复**: 添加 `PipelineStateDesc::colorLoadOp/depthLoadOp`，粒子 PSO 设置 `LoadOp::Load`
- 详见 [[loadop-fix]]

### 2. Push Constant 布局不匹配（核心问题）
- **现象**: GPU shader 完全不写入 counters，`renderCount` 永远为 0
- **根因**: C++ 使用 `glm::vec3`（MSVC alignas(16)），SPIR-V 使用 std140 布局。`sizeof(GpuEmitParam)` C++=144 vs SPIR-V 预期=128。所有字段偏移错位，Cull shader 读到垃圾 viewProj 矩阵剔除所有粒子
- **修复**: 重写 push constant 结构体使用原始 `float[3]` + 显式 `_pad` 字段，用 `spirv-cross --reflect` 验证每个偏移
- **教训**: push constant 结构体永远不要直接用 glm 类型，使用原始数组 + memcpy 赋值

### 3. VulkanBuffer Map/Unmap 空操作
- `Map()` 需要 `vkInvalidateMappedMemoryRanges`（GPU→CPU）
- `Unmap()` 需要 `vkFlushMappedMemoryRanges`（CPU→GPU）
- Intel Arc B370 内存是 coherent（BAR），但为兼容性始终调用

### 4. Init 重复执行
- 时间判断 `elapsed < dt*1.1f` 在 dt 很小时跨帧成立
- 改用 `bool initDone` flag

### 5. 首帧 dt=0
- `glfwGetTime()` 首帧差值可能为 0
- 兜底: `if (dt <= 0) dt = 1/60`

## 文件
- `Engine/Scene/Scene/ParticleComponent.h` — GPU 结构体定义（push constant 布局）
- `Engine/Render/Pipeline/ParticleRenderer.h/cpp` — 管线实现
- `Engine/Render/Pipeline/DeferredPipeline_FrameGraph.cpp` — FrameGraph 集成
- `Engine/Shader/Shaders/Particles/*.slang` — Slang shader 源码
- [[shadow-system-phase1]]
