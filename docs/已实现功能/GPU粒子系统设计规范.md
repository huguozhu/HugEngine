# GPU 粒子系统设计规范

> 参考 SeekEngine 实现，适配 HugEngine DeferredPipeline（VK 1.3 + Compute Shader + Slang + Bindless + Indirect Draw）

## 1. 目标

实现 GPU 驱动的粒子系统 ParticleComponent，在大规模粒子（10 万+）下保持高性能，支持火焰/烟雾/雨雪等效果。

## 2. 架构概述

```
CPU 端                              GPU 端
──────                              ──────
ParticleComponent                   Particle Pool (SSBO, MaxParticles × 32B)
  ├── ParticleSystemParam           DeadList[MaxParticles] (环形缓冲)
  ├── State: Stopped/Playing/Pause  Counters (原子操作)
  └── GPU Buffers                   AliveIndices[2] (双缓冲)
                                    SortIndices[SortCapacity]
 每帧 GPU Tick:                     IndirectArgs (Draw + Dispatch)
   TickBegin → Emit → Simulate
   → Culling → [Sort] → Render

 集成到 DeferredPipeline:
   GBuffer → [ParticleRender] → Lighting → PostProcess
```

## 3. 关键数据结构

### 3.1 GPU 端 (Slang — ParticleTypes.slang)

```hlsl
// 粒子数据 (32 bytes, 缓存友好)
struct Particle {
    float3 life_time;       // x: 当前生命, y: 总生命, z: 纹理时间
    uint   tex_index;       // 纹理子帧索引
    float4 velocity;        // xyz: 速度, w: 阻尼
    float4 position;        // xyz: 世界位置, w: 大小
};

// 全局计数器 (原子操作)
struct ParticleCounters {
    uint dead_count;           // DeadList 中可用数量
    uint alive_count[2];       // [0]=pre_sim, [1]=post_sim
    uint emit_count;           // 本帧发射数
    uint simulate_count;       // 本帧模拟数
    uint render_count;         // 本帧渲染数
    uint2 pad;
};

// 排序条目 (8 bytes)
struct SortInfo {
    uint particle_index;
    float particle_depth;
};
```

### 3.2 CPU 端 (C++ — ParticleComponent.h)

```cpp
// 发射参数
struct GpuEmitParam {
    float3 position;  uint   max_particles;
    uint   emit_direction_type;  float3 direction;
    float  direction_spread_percent;
    float  min_init_speed;  float max_init_speed;
    float  min_life_time;   float max_life_time;
    float3 box_size;  int  emit_shape;  float sphere_radius;
    uint2  tex_rows_cols;  uint tex_time_sampling_type;  float3 pad;
};

// 模拟参数
struct GpuSimulateParam {
    float  delta_time;  float3 gravity;
    uint   tex_time_sampling_type;  uint2 tex_rows_cols;
    float  tex_frames_per_sec;
};

// 渲染参数
struct GpuRenderParam {
    float4x4 view_matrix;  float4x4 proj_matrix;
    uint2    tex_rows_cols;  uint2 pad;
};

// Dispatch 间接参数
struct DispatchArgs {
    uint dispatch_num_x, dispatch_num_y, dispatch_num_z;
};

// Draw 间接参数
struct ParticleDrawArgs {
    uint count;  uint instance_count;
    uint first;  uint base_instance;
};
```

### 3.3 配置参数 (ParticleSystemParam)

```cpp
struct ParticleSystemParam {
    float   duration;                // -1 = 无限
    float   particles_per_sec;       // 发射速率
    float   min_life_time, max_life_time;
    float   min_init_speed, max_init_speed;
    EmitShapeType     emit_shape;    // Sphere / Box
    float3            box_size;
    float             sphere_radius;
    EmitDirectionType emit_direction; // Directional / Uniform2D / Uniform3D
    float3            direction;
    float             direction_spread;
    float3            gravity;
    uint2             tex_rows_cols;  // 纹理子帧网格
    float             tex_frames_per_sec;
    TexTimeSampling   tex_sampling;   // Once / Loop / Random
    SizeOverLife      size_over_life;       // 渐变曲线 (采样到 1D 纹理)
    ColorOverLife     color_over_life;      // 渐变色 (采样到 1D 纹理)
    TextureHandle     particle_tex;         // bindless 纹理
};
```

## 4. GPU Pass 详解

### 4.1 ParticleInit.comp — 初始化
```
Dispatch: Indirect
输入: MaxParticles (PushConstant)
输出: DeadList[i]=i, Counters={dead=MaxParticles, alive[0]=0, alive[1]=0, ...}
```

### 4.2 ParticleTickBegin.comp — 帧首
```
Dispatch: 1,1,1 (单线程组)
输入: emit_count (PushConstant)
输出: Counters.alive_pre=0, Counters.emit=emit_count
       IndirectArgs (Draw=0, EmitDispatch, SimDispatch=alive_post)
```

### 4.3 ParticleEmit.comp — 发射
```
Dispatch: Indirect (emit_dispatch)
输入: DeadList, GpuEmitParam, RandomFloats
输出: 从 DeadList 原子弹出 → 初始化 Particle → 写 alive_pre_indices
步骤:
  1. InterlockedAdd(dead_count, -1) 获取空槽
  2. 从 DeadList[slot] 读粒子索引
  3. 随机生成位置/方向/速度/生命
  4. 写入 ParticleBuffer[index]
  5. InterlockedAdd(alive_pre_count, 1) 写 alive_pre_indices
```

### 4.4 ParticleSimulate.comp — 模拟
```
Dispatch: Indirect (sim_dispatch)
输入: alive_pre_indices, ParticleBuffer, GpuSimulateParam, RandomFloats
输出: Particle 更新 (位置+=速度*dt, 生命-=dt, 纹理帧)
       死粒子索引 → DeadList (原子归还)
       活粒子 → alive_post_indices
```

### 4.5 ParticleCulling.comp — 剔除
```
Dispatch: Indirect (sim_dispatch)
输入: alive_post_indices, ParticleBuffer
输出: SortIndices (填充粒子索引+深度)
       视锥+距离剔除 → 写 DrawIndirectArgs
```

### 4.6 ParticleSort.comp — 深度排序
```
仅在有纹理时执行 (半透明需要排序)
算法: Bitonic Sort (块内 512) + 矩阵转置 (>512)
输入: SortIndices[] UAV, Counters
输出: 按深度降序排列的 SortIndices
```

### 4.7 ParticleRender — 渲染
```
类型: Graphics Pipeline
顶点: 预定义 6 顶点 (2 三角形 → Billboard 四边形)
      VS 根据粒子位置+大小扩展为面向相机的四边形
片元: 从 ColorOverLife/SizeOverLife 纹理采样
      可选: 纹理子帧动画
写入: GBuffer (Albedo + Emissive)
```

## 5. 集成到 DeferredPipeline

在 BuildFrameGraph 中新增 Pass：

```
DeferredPipeline::BuildFrameGraph:
  ...
  Lighting → [ParticleRender] → PostProcess
```

ParticleRender Pass 读 GBuffer 深度做软粒子混合，写 HDR Target。

## 6. 文件清单

### 新增
```
Engine/Scene/Scene/ParticleComponent.h      组件定义 + ParticleSystemParam
Engine/Scene/Scene/ParticleComponent.cpp     CPU 端管理 (Play/Stop/Tick/GPU Sync)

Engine/Shader/Shaders/Particles/
  ParticleTypes.slang                        GPU 结构体 (与 C++ 共享)
  ParticleInit.comp                          初始化
  ParticleTickBegin.comp                     帧首重置
  ParticleEmit.comp                          发射
  ParticleSimulate.comp                      模拟
  ParticleCulling.comp                       剔除
  ParticleSort.comp                          Bitonic 排序
  ParticleRender.vert + .frag                Billboard 渲染

Engine/Render/Pipeline/ParticleRenderer.h   渲染器封装 (管理 PSO + Buffers)
Engine/Render/Pipeline/ParticleRenderer.cpp
```

### 修改
```
Engine/Render/Pipeline/DeferredPipeline.h   新增 ParticleRenderer 成员
Engine/Render/Pipeline/DeferredPipeline.cpp  新增 ParticleRender Pass
Engine/Scene/Scene/SceneReflect.cpp          注册 ParticleComponent
Engine/Render/CMakeLists.txt                 新增源文件
Engine/Shader/CMakeLists.txt                 新增 Shader
```

## 7. 与 SeekEngine 实现差异

| 方面 | SeekEngine | HugEngine |
|------|-----------|-----------|
| 着色器语言 | .slang (SeekEngine 方言) | .comp/.vert/.frag (Slang) |
| 渲染集成 | 独立 Draw | DeferredPipeline GBuffer / HDR |
| 描述符 | 手动 SetParam | Bindless 纹理数组 |
| 排序 | PreSort + Bitonic + Transpose (3 kernel) | 合并为单个 BitonicSort |
| 软粒子 | 不支持 | 读 GBuffer 深度，按距离淡出 |
| Mesh 粒子 | 不支持 | MeshShader instanced meshlet |

## 8. 里程碑

| # | 内容 | 验证标准 |
|:---:|------|----------|
| P1 | ParticleComponent + Init/Emit/Simulate Compute | GPU 计数器正确，Debug 输出 |
| P2 | Billboard 渲染 + 最简单粒子效果 | 屏幕可见运动粒子 |
| P3 | ColorOverLife + SizeOverLife + 纹理序列帧 | 粒子有颜色/大小/纹理变化 |
| P4 | 深度排序 (Bitonic Sort) | 半透明粒子正确排序 |
| P5 | 软粒子 + GBuffer 深度混合 | 粒子与几何体边缘平滑 |
| P6 | DeferredPipeline 集成 | 火焰/烟雾/雨雪效果 |
| P7 | Mesh 粒子 (MeshShader instanced) | 碎片/弹壳效果 |
