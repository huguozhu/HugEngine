# GPU Driven Deferred Rendering 完整方案设计

> 版本: 1.0 | 日期: 2026-07-14 | 基于 HugEngine Phase 2 ~80% 完成度

## 1. 目标

将 HugEngine 的 DeferredPipeline 从当前 ~80% GPU Driven 完成度提升至 100%，实现 CPU 完全不介入绘制命令生成的现代化 GPU Driven 渲染管线。

## 2. 当前状态

### 已完成

| 功能 | 状态 |
|------|:---:|
| GPU 视锥剔除 (Compute Shader) | ✅ |
| Hi-Z 遮挡剔除 (单阶段) | ✅ |
| ExecuteIndirect (DrawIndexedIndirect) | ✅ |
| GPU Scene Upload (GPUScene) | ✅ |
| Bindless 纹理/资源 | ✅ |
| MeshBatcher (GPU 批量绘制) | ✅ |
| Clustered Shading | ✅ |
| Timeline Semaphore + 双队列 | ✅ |

### 已知问题

| 问题 | 影响 |
|------|------|
| GPUCull dispatch 在 RenderPass 内 | Vulkan 校验警告 |
| AsyncCompute 默认关闭 | 并发能力未充分利用 |
| 单阶段 Hi-Z 有 false positive | 遮挡剔除精度不够 |
| 每帧 Dispatch 有调度开销 | 不必要的 GPU 开销 |

## 3. 实施方案：里程碑 C

按"稳定性 → 质量 → 自主化 → 补齐"逻辑分 4 个里程碑：

```
M1 · 修复与稳定 → M2 · 剔除质量 → M3 · GPU 命令自主化 → M4 · 补齐
```

### 3.1 M1 · 修复与稳定

**Item 3: GPUCulling Dispatch 移出 RenderPass**

现状问题：
```
RenderPass::Begin (GBuffer)
  ├── vkCmdDispatch (GPUCull)        ← 违规：Compute 在 RenderPass 内
  ├── vkCmdDrawIndexedIndirect
  └── RenderPass::End
```

目标方案：
```
vkCmdDispatch (GPUCull)              ← Barrier: Depth → Compute Read
Barrier (Compute Write → Indirect Draw Read)
RenderPass::Begin (GBuffer)
  ├── vkCmdDrawIndexedIndirect
  └── RenderPass::End
```

改动点：
- `DeferredPipeline::BuildFrameGraph` 中调整 GPU_Cull Pass 与 GBuffer Pass 的执行时序
- 确保在 GBuffer `BeginRenderPass` 之前完成 GPUCulling compute dispatch
- 无需改动 RenderGraph 基础设施，仅在 Pipeline 层面调整执行顺序

**Item 6: AsyncCompute 完善 + 默认开启**

目标架构：
```
Frame N:
  Graphics Queue                    Async Compute Queue
  ──────────────                    ───────────────────
  Shadow Pass                       GPUCull (读上帧深度)
  ├── Signal Timeline(N) ──────────► Wait Timeline(N)
  │                                 SSGI (读 GBuffer N-1)
  │                                 SSAO (读 GBuffer N-1)
  │                                 DDGI Update
  │                                 AutoExposure (读 HDR N-1)
  │                                 ◄── Signal Timeline(N+1)
  Wait Timeline(N+1) ──────────────
  GBuffer Pass
  Lighting Pass
  Bloom / DOF / MB
  TAA / ToneMap / FXAA
  Present
```

改动点：
- 将以下 Compute Pass 标记为 Async 并移到 Async Compute Queue：
  - GPUCulling（读上帧深度，与当前帧 GBuffer 无依赖）
  - SSAO / SSGI / SSR（屏幕空间效果，可异步）
  - DDGI Update（探针更新，可异步）
  - AutoExposure（亮度统计，可异步）
- Timeline Semaphore 确保 Graphics Queue 写入在 Async 读取之前完成
- 设置 `m_UseAsyncCompute = true` 作为默认值

### 3.2 M2 · 剔除质量

**Item 2: Two-Phase Occlusion Culling**

两阶段流水线：
```
Phase 1 (粗筛): 复用上帧 Hi-Z
  ├── 读: HiZ_N-1 (上一帧深度金字塔)
  ├── 保守策略: 不确定的全部 pass
  └── 输出: Phase2CandidateBuffer

Phase 2 (精筛): 当前帧 Hi-Z 验证
  ├── 仅处理 Phase 1 通过的 Object
  ├── 读: HiZ_N (当前帧深度金字塔)
  └── 输出: IndirectDrawBuffer (最终绘制命令)
```

改动点：
- 新增 `GPUCull_TwoPhase.comp.slang` 或扩展现有 `GPUCull.comp.slang`
- `GPUCulling` 类增加 `m_UseTwoPhase` 开关和 Phase 2 PSO
- GPUScene 额外存储 Phase 1 中间结果 buffer
- Phase 1 可与 GBuffer 并行（Async Compute Queue）

**Item 4: Persistent Thread Group**

传统模式 vs PTG 模式：
```
传统: Frame N: vkCmdDispatch → [处理] → 退出
      Frame N+1: vkCmdDispatch → ...

PTG:  Init: Dispatch(N_PTG) ← 一次性
      Frame N: Signal → [处理] → SpinWait
      Frame N+1: Signal → [处理] → SpinWait
```

实现要点（HLSL 伪码）：
```hlsl
[numthreads(64, 1, 1)]
void PersistentCull() {
    while (true) {
        // 等待新帧信号
        uint frameIndex;
        while ((frameIndex = u_FrameSignal[0]) == g_LastFrameIndex) { /* spin */ }
        g_LastFrameIndex = frameIndex;
        
        // 处理本线程组负责的 Object
        for (uint i = gtid; i < totalObjects; i += PTG_TOTAL_THREADS) {
            // ... 视锥 + 遮挡剔除逻辑 ...
        }
        DeviceMemoryBarrier();
    }
}
```

改动点：
- 新增 `PersistentCull.comp.slang` 着色器
- `GPUCulling` 类增加 PTG 模式初始化/关闭
- 启动时 Dispatch 一次，运行时通过 `u_FrameSignal` buffer 触发
- PTG 是纯软件模式：标准 Compute Shader + GPU 端 spin-wait，无需特殊硬件扩展
- 需控制单帧处理时间避免 TDR（shader 内限制循环迭代次数，超时主动 yield）

### 3.3 M3 · GPU 命令自主化

**Item 1: Device Generated Commands (VK_EXT_device_generated_commands)**

DGC 核心概念：
```
IndirectCommandsLayout     ← 定义 GPU 可生成的命令序列
Preprocess Buffer (GPU写)  ← 每个 Object → 一个 Command Sequence
Sequences Buffer (GPU执行) ← Device 遍历 → 执行每个 Draw
vkCmdExecuteGeneratedCommandsEXT ← CPU 唯一的 DGC 调用
```

流程对比：
```
现状: CPU vkCmdDispatch(GPUCull) → CPU vkCmdDrawIndexedIndirect
目标: CPU vkCmdDispatch(GPUCull) → CPU vkCmdExecuteGeneratedCommandsEXT
                                    (Draw 命令由 GPU 生成)
```

所需 Vulkan 扩展（DGC 相关）：
- `VK_EXT_device_generated_commands` — 核心 DGC 功能
- `VK_KHR_buffer_device_address` — GPU 端 buffer 地址访问
- `VK_EXT_descriptor_buffer`（可选）— DGC 序列中嵌入描述符绑定

实现步骤：
1. 查询 `VK_EXT_device_generated_commands` 支持
2. 创建 `IndirectCommandsLayoutEXT`（定义 GPU 可生成的 token 序列）
3. `MeshBatcher` 增加 DGC 路径：`vkCmdDrawIndexedIndirect` → `vkCmdExecuteGeneratedCommandsEXT`
4. GPU Culling Shader 输出直接写入 Preprocess Buffer（替代原有 IndirectDraw buffer）
5. 保留传统 ExecuteIndirect fallback 路径（设备不支持 DGC 时自动切换）

**Item 5: GPU Work Graphs**

Work Graph 概念（Cull → Draw → PostProcess）：
```
┌──────────┐  records[]   ┌──────────┐  records[]   ┌──────────┐
│ Entry    │ ───────────► │ DrawNode │ ───────────► │ PostNode │
│ (Compute)│              │ (Mesh)   │              │ (Compute)│
└──────────┘              └──────────┘              └──────────┘
```

实现策略：
| 路径 | API | 状态 |
|------|-----|------|
| D3D12 Work Graphs | ID3D12WorkGraph | 仅 Windows |
| Vulkan | VK_AMDX_shader_enqueue | AMD 专有 |
| 软件模拟 | Compute + 原子计数器 | 全平台 |

推荐先用软件模拟路径实现核心概念，后续根据硬件支持情况接入原生 API。

改动点：
- 新增 `GPUWorkGraph` 基础设施类
- 定义 Node 类型：`Entry` / `Compute` / `Draw`
- 实现 GPU 端 Record 传递（buffer 队列 + 原子计数器）
- 与 GPUCulling 集成：Cull Node → Draw Node

### 3.4 M4 · 补齐

**Item 7: Forward+ Pipeline**

改动点：
- `ForwardPipeline` 接入已有的 `ClusteredShading` 类
- Forward PBR Shader 增加 LightGrid 查找路径（Shader 变体）
- `ForwardPipeline::BuildFrameGraph` 增加 ClusteredLightCulling dispatch
- ClusteredShading 类已独立可用，改动量最小

## 4. 文件变更清单

### 新增文件

| 文件 | 里程碑 | 说明 |
|------|:---:|------|
| `Engine/Shader/Shaders/GPUCull_TwoPhase.comp.slang` | M2 | 两阶段遮挡剔除 Compute Shader |
| `Engine/Shader/Shaders/PersistentCull.comp.slang` | M2 | 持久化线程组剔除 Shader |
| `Engine/Shader/Shaders/DGC_Generate.comp.slang` | M3 | DGC 命令序列生成 Compute Shader |
| `Engine/RHI/DGC/` | M3 | DGC 抽象层（类似 RT 目录结构） |
| `Engine/Render/Pipeline/GPUWorkGraph.h` | M3 | GPU Work Graph 基础设施 |
| `Engine/Render/Pipeline/GPUWorkGraph.cpp` | M3 | GPU Work Graph 实现 |

### 修改文件

| 文件 | 里程碑 | 改动 |
|------|:---:|------|
| `DeferredPipeline.cpp` | M1-M3 | 执行时序调整 + Async 标记 + DGC 集成 |
| `GPUCulling.h/.cpp` | M1-M3 | 两阶段 + PTG + DGC 输出 |
| `GPUScene.h/.cpp` | M2 | Phase 1 中间 buffer |
| `MeshBatcher.h/.cpp` | M3 | DGC 路径 |
| `ForwardPipeline.cpp` | M4 | ClusteredShading 集成 |
| `RenderGraph.h/.cpp` | M1 | Async pass 标记细化（Item 6 所需，Item 3 无需改动 RG） |
| `RHI/Vulkan/VulkanDevice.cpp` | M3 | DGC 扩展查询 + 功能检测 |

## 5. 设备兼容性

| 功能 | 最低要求 | 回退方案 |
|------|----------|----------|
| DGC | Vulkan 1.3.296+ | 传统 ExecuteIndirect |
| Work Graphs (D3D12) | SM 6.9+, 仅 Windows | 软件模拟路径 |
| Work Graphs (Vulkan) | VK_AMDX_shader_enqueue (AMD) | 软件模拟路径 |
| AsyncCompute | Vulkan 所有现代 GPU | 同步执行 |
| Persistent Thread Group | 所有支持 Compute 的设备 | 传统 Dispatch |

## 6. 验证方式

| 里程碑 | 验证项 | 方法 |
|--------|--------|------|
| M1 | Vulkan 校验层干净 | `--validate` 运行 04.Deferred，零警告 |
| M1 | AsyncCompute 开启 | GPU Profiler 面板确认 Async 队列有活动 |
| M2 | Two-Phase 剔除精度 | Debug View 对比单/双阶段剔除结果 |
| M2 | PTG 帧率提升 | Sponza 场景 before/after Dispatch 次数对比 |
| M3 | DGC 正确性 | 与 ExecuteIndirect 结果像素级对比 (PSNR > 50dB) |
| M3 | Work Graph fallback | 软件模拟路径渲染正确 |
| M4 | Forward+ 光照 | 与 Deferred 光照结果视觉一致 |

## 7. 风险与缓解

| 风险 | 概率 | 缓解 |
|------|:---:|------|
| VK_EXT_dgc 驱动不稳定 | 中 | 保留 ExecuteIndirect fallback，通过 CVar 切换 |
| PTG 触发 TDR | 低 | 限制单帧处理时间，超过阈值自动切回传统 Dispatch |
| Work Graph 软件模拟性能不如原生 | 中 | 设计为可替换后端，后续接入原生 API |
| AsyncCompute 跨队列同步复杂 | 中 | 充分的 Timeline Semaphore 验证 + 校验层 |

## 8. 工期预估

```
M1 · 修复与稳定:  3-5 天
M2 · 剔除质量:    5-7 天
M3 · GPU 命令自主化: 10-14 天
M4 · 补齐:        2-3 天
─────────────────────────
总计:            20-29 天 (约 4-6 周)
```
