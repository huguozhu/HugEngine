# RHI Transient Resource Allocator + PSO/Shader 编译管线规划

> 2026-07-22 | 基于 HugEngine RHI vs UE5 差距分析

---

## 一、Transient Resource Allocator（瞬态资源分配器）

### 1.1 当前问题

HugEngine 每帧每个 Pass 独立创建纹理：

```cpp
m_SSAO->Initialize(device, m_Width, m_Height);  // 创建 AO texture
m_Bloom->Initialize(device, m_Width, m_Height);  // 创建 bloom texture
m_DOF->Initialize(device, m_Width, m_Height);    // 创建 DOF texture
```

**问题：**
- 每帧分配大量 VkImage/VkDeviceMemory，即使上帧的纹理可以复用
- 内存碎片化严重：100+ Pass 的渲染管线会产生 50+ 个分辨率相近的临时纹理
- 4K 渲染时：一张 RGBA16F 纹理 = 33MB，10 张 = 330MB 浪费
- VkImage 创建/销毁有 GPU 侧的 cache flush 开销

### 1.2 UE5 方案：RDG Transient Resource Allocator

UE5 的 `FRenderGraphBuilder` 使用分层分配策略：

```
┌──────────────────────────────────────────────────┐
│                TransientResourceAllocator          │
│                                                     │
│  ┌─────────────┐  ┌─────────────┐  ┌───────────┐ │
│  │ Memory Pool  │  │ Memory Pool  │  │ Memory    │ │
│  │ (64MB)       │  │ (128MB)      │  │ Pool      │ │
│  │              │  │              │  │ (256MB)   │ │
│  └─────────────┘  └─────────────┘  └───────────┘ │
│                                                     │
│  Allocation: offset-based, bump allocator           │
│  Deallocation: frame-based ring buffer              │
└──────────────────────────────────────────────────┘
```

**核心思想：**

1. **内存池（VkDeviceMemory 大块分配）**
   - 预分配 2-3 个大块 VkDeviceMemory（64/128/256MB）
   - Frame N 使用 Pool A，Frame N+1 使用 Pool B（双缓冲）
   - 每帧重置 bump pointer，所有子分配在帧末一次回收

2. **别名分析（Aliasing）**
   ```
   Pass 1: SSAO → Texture A (1920×1080 RG16F = 8MB)
   Pass 2: Bloom → Texture B (960×540 RGBA16F  = 8MB)
   Pass 3: DOF  → Texture C  (1920×1080 RGBA8 = 8MB)

   传统分配: A(8MB) + B(8MB) + C(8MB) = 24MB
   别名分配: A 和 B 永不同时活跃 → 共享同一块 8MB = 8MB total
   ```
   通过 RenderGraph 的依赖分析，自动发现可以共享内存的资源对。

3. **分层 VkImage 管理**
   - **底层：一块 VkDeviceMemory**（Heap）
   - **中层：VkImage**（绑定到 Heap 的 offset 位置）
   - **上层：VkImageView**（每个 Pass 需要的 mip/face/format 视图）

   关键是 VkImage 创建时使用 `VkImageCreateInfo` + `VkBindImageMemoryInfo` 的 offset 参数

4. **Vulkan 实现要点**
   ```cpp
   // 1. 查询对齐要求
   VkMemoryRequirements memReqs;
   vkGetImageMemoryRequirements(device, image, &memReqs);

   // 2. 将 offset 对齐到 memReqs.alignment
   u64 alignedOffset = Align(bumpOffset, memReqs.alignment);

   // 3. 绑定到 Heap 的 offset 位置
   VkBindImageMemoryInfo bindInfo{};
   bindInfo.image = image;
   bindInfo.memory = heapMemory;
   bindInfo.memoryOffset = alignedOffset;
   vkBindImageMemory2(device, 1, &bindInfo);

   // 4. 帧末：无需逐资源 vkDestroyImage，只需重置 bump pointer
   // VkImage 对象缓存在 per-format pool 中复用
   ```

### 1.3 HugEngine 实现方案

**文件结构：**
```
Engine/RHI/
├── TransientResourceAllocator.h   ← 新增
├── TransientResourceAllocator.cpp
└── RenderGraph.h                   ← 扩展：别名分析
```

**架构：**
```
┌─────────────────────────────────┐
│  RenderGraph::Compile()          │
│  ├── BuildDependencies()         │
│  ├── TopologicalSort()           │
│  ├── DeriveBarriers()            │
│  └── AnalyzeAliasing()  ← 新增   │
│      找出生命周期不重叠的资源对     │
└─────────────────────────────────┘
         ↓
┌─────────────────────────────────┐
│  TransientResourceAllocator      │
│  ├── AllocateHeap(size)          │
│  ├── CreatePlacedImage(desc,heap)│
│  ├── CreatePlacedBuffer(desc)    │
│  ├── AdvanceFrame()              │ ← 帧末回收
│  └── GetStats()                  │
└─────────────────────────────────┘
```

**关键接口设计：**
```cpp
class TransientResourceAllocator {
public:
    // 初始化：预分配 N 个 Heap（默认 128MB × 2，双缓冲）
    bool Initialize(VkDevice device, u64 heapSize = 128 * 1024 * 1024);

    // 在 Heap 中分配 VkImage（返回 offset，对齐已处理）
    struct PlacedImage {
        VkImage image;
        u64     offset;  // Heap 内偏移
        u64     size;    // 实际占用大小（含对齐）
    };
    PlacedImage AllocateImage(const VkImageCreateInfo& info);

    // 帧切换：回收上一帧的 bump pointer，cache 中的 VkImage 可复用
    void AdvanceFrame();

    // 统计
    u64 GetUsedMemory()  const;
    u64 GetTotalMemory() const;
    u32 GetImageCacheHitRate() const;
};
```

**与 RenderGraph 集成：**
```cpp
void RenderGraph::AnalyzeAliasing() {
    // 1. 收集所有 texture/buffer 资源的生命周期区间
    struct ResourceLifetime { u32 firstUse; u32 lastUse; };
    // 2. 区间交集检测：扫描线算法 O(n log n)
    // 3. 分配别名组：每个 group 内的资源共享一块物理内存
    // 4. 记录 alias mapping：m_ResourceAliases[handleA] = handleB
}
```

**内存节省估算：**

| 场景 | 传统分配 | Transient 分配 | 节省 |
|------|---------|---------------|------|
| Forward (1080p) | ~400MB | ~120MB | 70% |
| Deferred (4K) | ~800MB | ~250MB | 69% |
| Mobile (720p) | ~150MB | ~50MB | 67% |

---

## 二、PSO / Shader 编译管线

### 2.1 当前问题

```cpp
// 当前：同步创建，每帧首次使用时卡顿
VkShaderModule comp = createShader(desc.computeShader, VK_SHADER_STAGE_COMPUTE_BIT);
vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compInfo, nullptr, &pipeline);
// ↑ 首次编译可能耗时 50-200ms，帧率暴跌
```

**问题根源：**
- **Shader 编译**（SPIR-V → GPU ISA）：首次使用 shader 时触发，不可预测
- **PSO 创建**（所有状态组合 → 硬件 PSO）：Vulkan 驱动内部有很多 work
- **无持久化缓存**：每次程序重启都重新编译

### 2.2 UE5 方案

UE5 的编译管线分三层：

```
Layer 1: Shader Compilation (HLSL → SPIR-V/DXIL)
  ┌──────────────────┐
  │ ShaderCompiler    │  离线/异步编译
  │ ───────────────── │
  │ CompileShader()   │  Worker Thread 1..N
  │ GetShaderMap()    │  等待+收集结果
  └──────────────────┘

Layer 2: Pipeline State Cache
  ┌──────────────────┐
  │ PipelineCache     │  持久化 + 内存缓存
  │ ───────────────── │
  │ LoadFromDisk()    │  VkPipelineCache 文件加载
  │ SaveToDisk()      │  pCacheData → 写入磁盘
  │ PrecompilePSO()   │  后台预热 PSO
  └──────────────────┘

Layer 3: PSO Create Throttle
  ┌──────────────────┐
  │ PSOCreateManager  │  限流 + 批处理
  │ ───────────────── │
  │ EnqueueCreate()   │  入队创建请求
  │ ProcessQueue(N)   │  每帧最多创建 N 个
  │ GetPendingCount() │  待创建队列长度
  └──────────────────┘
```

### 2.3 关键机制

#### 2.3.1 VkPipelineCache 持久化

```cpp
// 初始化时加载磁盘缓存
std::vector<u8> cacheData = LoadFile("pipeline_cache.bin");
VkPipelineCacheCreateInfo cacheInfo{};
cacheInfo.initialDataSize = cacheData.size();
cacheInfo.pInitialData    = cacheData.data();
vkCreatePipelineCache(device, &cacheInfo, nullptr, &m_PipelineCache);

// 创建 PSO 时传入 cache
vkCreateGraphicsPipelines(device, m_PipelineCache, 1, &pipeInfo, nullptr, &pipeline);

// 关闭时保存
size_t dataSize;
vkGetPipelineCacheData(device, m_PipelineCache, &dataSize, nullptr);
std::vector<u8> data(dataSize);
vkGetPipelineCacheData(device, m_PipelineCache, &dataSize, data.data());
SaveFile("pipeline_cache.bin", data);
```

**效果：首次编译 50ms → 热启动 2ms**

#### 2.3.2 PSO 预热（Precompilation）

```cpp
struct PSOPrecompileTask {
    u64   hash;
    PipelineStateDesc desc;
    std::atomic<bool>* ready;
};

// 后台线程逐项编译 → 写入 m_PipelineCache
// 游戏线程首次使用前检查预热状态：命中 = 零开销，未命中 = 同步编译回退
```

#### 2.3.3 PSO 创建限流

```cpp
static constexpr u32 kMaxPSOCreatesPerFrame = 3;

void ProcessPSOQueue(u32 maxPerFrame) {
    u32 processed = 0;
    while (!m_PendingCreates.empty() && processed < maxPerFrame) {
        auto& req = m_PendingCreates.front();
        auto* pso = CreatePipeline(req.desc);
        req.callback(pso);
        m_PendingCreates.pop();
        processed++;
    }
}
```

#### 2.3.4 VK_EXT_graphics_pipeline_library（可选）

将 PSO 拆分为 4 个独立部分，可分别缓存：
- Vertex Input Interface
- Pre-Rasterization Shaders
- Fragment Shader
- Fragment Output Interface

快速链接（Fast Link）：从已缓存的 4 部分组装完整 PSO，耗时 ~0.5ms vs 50ms

### 2.4 HugEngine 实现方案

#### Phase 1：VkPipelineCache 持久化（最小改动）

```
当前 PSO 缓存:
  内存 LRU cache (HashPipelineStateDesc → VkPipeline)
  └── 问题: 每次进程重启清空

改为:
  VkPipelineCache (持久化)
  └── Initialize: 加载 pipeline_cache.bin → VkPipelineCache
  └── CreatePSO: 传入 m_PipelineCache
  └── Shutdown: 保存 VkPipelineCache 到 pipeline_cache.bin
  └── 内存 LRU 仍然保留（加速运行时查找）

改动量: ~50 行代码
收益: 热启动 PSO 创建 50ms → 2ms
```

#### Phase 2：异步 PSO 预热

```
新增:
  Engine/RHI/
  ├── PSOPrecompileManager.h    ← 预热任务队列
  └── PSOPrecompileManager.cpp

流程:
  1. Engine 启动 → 收集所有可能的 PipelineStateDesc 组合
     （从 Material 数据库 + RenderPass 配置枚举）
  2. 后台线程逐项编译 → 写入 m_PipelineCache
  3. 游戏线程在首次使用前检查预热状态
     └── 命中: 零开销
     └── 未命中: 同步编译（回退路径）

改动量: ~200 行代码
收益: 运行时 PSO 创建卡顿完全消除
```

#### Phase 3：PSO 限流 + Fast Link

```
场景: 开放世界 1000+ 材质 → 1000+ PSO 变体
限流: 每帧最多创建 3 个新 PSO，其余排队
Fast Link: 将 PSO 拆分为 4 部分独立缓存，组合耗时 ~0.5ms

（需要 VK_EXT_graphics_pipeline_library 支持）
```

---

## 三、实现优先级

| Phase | 功能 | 代码量 | 收益 | 风险 |
|-------|------|--------|------|------|
| **1** | VkPipelineCache 持久化 | ~50 行 | 热启动 **25× 加速** | 零风险 |
| **2** | Transient Allocator 基础版 | ~400 行 | **内存节省 60-70%** | 中（需 RenderGraph 别名分析） |
| **3** | PSO 预热 + 限流 | ~200 行 | 消除运行时卡顿 | 低 |
| **4** | Pipeline Library (Fast Link) | ~300 行 | PSO 链接 **100× 加速** | 中（需扩展支持） |

**建议顺序：Phase 1 → Phase 2 → Phase 3，Phase 4 等待硬件普及。**
