# Pipeline Library (Fast Link) + PSO 限流器设计

> **日期**: 2026-08-13
> **状态**: 已批准
> **目标**: 完成 `docs/未实现功能/RHI瞬态分配器与PSO编译管线规划.md` 中 Phase 4（Pipeline Library / Fast Link）与「PSO 限流器（未实现）」两项，并附 CVar 门控变体演示 Pass。

## 背景

规划文档中 Phase 1（VkPipelineCache 持久化）、Phase 2/2.5（Transient Allocator）、Phase 3（PSO 预热管理器）均已完成，仅剩：

1. **Phase 4 Pipeline Library (Fast Link)**：基于 `VK_EXT_graphics_pipeline_library`，将 Graphics PSO 拆分为 4 个独立部分分别缓存，变体场景下 link 耗时 ~0.5ms vs 单片 50ms。
2. **PSO 限流器（未实现）**：规划 Phase 3 末尾明确「待未来材质变体系统（1000+ PSO）时再实现」。

本设计一次补齐两者，并用一个 CVar 门控的变体演示 Pass 验证 GPL fast-link 真实生效（而非仅编译通过）。

**已验证硬件**：开发机 RTX 4060 Laptop（Ada Lovelace），`graphicsPipelineLibrary` 与 `graphicsPipelineLibraryFastLinking` 两个特性均支持，演示可真实跑通计时。

**启用策略**：检测到 GPL 支持时 fast-link 路径**自动启用**；现有单变体 PSO（SSAO 等）与不支持 GPL 的后端**自动回退**单片路径，零行为变化。

---

## 一、能力检测（照 `VulkanDevice_MeshShader.cpp` 范式）

### 现状

引擎已有成熟扩展检测范式：`m_SupportsRT/Mesh/DGC` bool 标志 + `Query*Capabilities()` + pNext 链追加 `VkPhysicalDevice*Features` 结构 + 条件 `deviceExtensions.push_back`（`VulkanDevice.cpp:403-552`、`VulkanDevice_MeshShader.cpp`）。

### 方案

- `VulkanDevice.h` 新增：
  - `bool m_SupportsGPL = false;`
  - `bool m_SupportsGPLFastLinking = false;`
  - `bool SupportsGraphicsPipelineLibrary() const` / `bool SupportsGPLFastLinking() const`
- 新增 `VulkanDevice_GPL.cpp` 的 `QueryGPLCapabilities()`：
  - `vkGetPhysicalDeviceFeatures2` 查询 `VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT.graphicsPipelineLibrary` → `m_SupportsGPL`
  - `vkGetPhysicalDeviceProperties2` 查询 `VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT.graphicsPipelineLibraryFastLinking` → `m_SupportsGPLFastLinking`（注意：fast-linking 在 **Properties** 结构，不在 Features 结构）
- `VulkanDevice.cpp`：条件追加两个设备扩展 `VK_KHR_pipeline_library`（依赖扩展）+ `VK_EXT_graphics_pipeline_library`，并将 `VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT{graphicsPipelineLibrary=VK_TRUE}` 按现有顺序追加进 pNext 链。

> **正确性关键**：`graphicsPipelineLibraryFastLinking` 决定 link 时是否必须带 `VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT`。支持 fast-linking 时**不带**该 bit（~0.5ms 快链）；不支持时**必须带**（退化为接近单片编译的正确路径，只保留"分库缓存"收益）。两分支都要实现。

---

## 二、PipelineLibraryCache（新增 `VulkanPipelineLibrary.h/.cpp`）

### 4 段拆分与缓存 key

4 个独立 `unordered_map<u64, VkPipeline>`，各自以「该段相关状态的 FNV-1a 哈希」为 key：

| 段 | 库位 | 包含状态 | 缓存 key |
|----|------|---------|---------|
| Vertex Input Interface | `VERTEX_INPUT_INTERFACE_BIT_EXT` | `pVertexInputState` | vertexLayout(stride+attributes) |
| Pre-Rasterization Shaders | `PRE_RASTERIZATION_SHADERS_BIT_EXT` | VS stage + IA + viewport + rasterizer + depthStencil + dynamic + layout | VS SPIR-V + topology + cull/frontFace/fill + depth 状态 + pushConstants |
| Fragment Shader | `FRAGMENT_SHADER_BIT_EXT` | FS stage + layout | FS SPIR-V + pushConstants |
| Fragment Output Interface | `FRAGMENT_OUTPUT_INTERFACE_BIT_EXT` | multisample + colorBlend + renderPass | colorFormats + blend + sampleCount |

### 接口

```cpp
class PipelineLibraryCache {
public:
    void Initialize(VkDevice device, VkPipelineCache cache,
                    DeferredDestructionQueue* deferredDestroy);
    void Shutdown();

    // 每段：查/建库（缺失时用 VK_PIPELINE_CREATE_LIBRARY_BIT_KHR 创建并缓存）
    VkPipeline GetOrCreateVertexInputLibrary(const VertexInputKey& key);
    VkPipeline GetOrCreatePreRasterLibrary(const PreRasterKey& key);
    VkPipeline GetOrCreateFragmentShaderLibrary(const FragmentShaderKey& key);
    VkPipeline GetOrCreateFragmentOutputLibrary(const FragmentOutputKey& key);

    // fast-link：组合 4 段产出完整 VkPipeline
    // @param linkTimeOptimize  fast-linking 不可用时置 true（带 LTO bit）
    VkPipeline LinkPipeline(const VkPipeline libs[4], VkPipelineLayout layout,
                            VkRenderPass renderPass, u32 subpass,
                            bool linkTimeOptimize);
};
```

- 每段 `vkCreateGraphicsPipelines` 均传入主 `VkPipelineCache`，与 Phase 1/3 驱动缓存共存。
- `Shutdown()` 统一销毁 4 段缓存中的 `VkPipeline`（走 `DeferredDestructionQueue` 延迟销毁）。

---

## 三、集成进 `CreateVulkanPipeline`

### 现状

`VulkanPipeline.cpp` 的 `CreateVulkanPipeline` 是单片创建：Compute / Mesh / 传统 VS+FS 三条路径，各自一次性 `vkCreateGraphicsPipelines`，结果插入 `m_PSOCache`（key = 完整 desc 哈希）。

### 方案

在现有 PSO 缓存查找之后、单片创建之前插入 GPL 分支（仅传统 VS+FS 图形管线且 `SupportsGPL()`）：

```
GPL 分支:
  1. 从 desc 派生 4 段的 key → GetOrCreate*Library（缺失才 vkCreate，带 LIBRARY_BIT）
  2. LinkPipeline(4 段, layout, renderPass, subpass, !fastLinking) → 完整 VkPipeline
  3. 插入现有 m_PSOCache（后续同 desc 请求直接命中全管线缓存，不再 re-link）
  其余（Compute / Mesh / 不支持 GPL / 任一段创建失败）→ 走原单片路径
```

- Mesh Shader 管线暂保持单片路径（GPL 支持 mesh/task 属后续扩展，本次不纳入）。
- 结果仍包装为 `VulkanPipelineState`，沿用现有「缓存模式」生命周期管理（`m_CacheRef` + `DeferredDestructionQueue`）。

---

## 四、PSO 限流器

### 现状

引擎所有 PSO 在 `Initialize()` 阶段一次性创建，帧循环无运行时 PSO 创建，故规划 Phase 3 未实现限流器。

### 方案

- `IRHIDevice`（`RHI/RHI.h`）新增默认空实现虚方法（沿用 precompile 范式）：
  - `virtual void EnqueuePSOCreate(const PipelineStateDesc& desc) {}`
  - `virtual void ProcessPSOCreateQueue(u32 maxPerFrame) {}`
  - `virtual u32 GetPendingPSOCreateCount() const { return 0; }`
- `VulkanDevice` 持 `std::deque<PipelineStateDesc> m_PendingCreates`；`ProcessPSOCreateQueue` 每帧从队列取最多 N 个，逐项调用 `CreatePipelineState`（返回值丢弃——PSO 缓存保留 Vulkan 对象，无泄漏）。
- `DeferredPipeline::NextFrame()` 调用 `ProcessPSOCreateQueue(kMaxPSOCreatesPerFrame)`，`kMaxPSOCreatesPerFrame = 3`。

---

## 五、变体演示 Pass（CVar `cvGPLVariantTest`）

仿 Phase 2.5 `cvTransientTest` 模式，默认关闭、设为 1 重新编译启用。

### 变体维度（已定）

生成 **N 个（默认 16）** 图形 PSO 变体：共享同一 VS + FS + vertex input + render pass，仅 **blend/color-output 状态不同**（零新着色器，最省事）。以此验证「vertex-input / pre-raster / fragment-shader 三段库各编译 1 次、fragment-output 段编译 N 次、N 次 fast-link」。

### 数据流

```
Initialize()（cvGPLVariantTest=1 且 SupportsGPL()）:
  构造 N 个 PipelineStateDesc（仅 colorBlend 状态不同）
  → EnqueuePSOCreate(desc) × N

NextFrame():
  ProcessPSOCreateQueue(3)  → 每帧最多 3 个
    └── CreatePipelineState → GPL fast-link（4 段库查/建 + LinkPipeline）

日志输出:
  各段库编译次数（pre-raster×1 / fragment-shader×1 / fragment-output×N）
  单次 link 耗时、N 变体总耗时 vs 单片基线估算
```

### CVar

| CVar | 类型 | 默认 | 说明 |
|------|------|------|------|
| `r.GPL.VariantTest` | bool | false | 变体演示开关（启用后生成 N 变体走 fast-link） |
| `r.GPL.VariantCount` | i32 | 16 | 变体数量 N |

---

## 六、错误处理与回退

- **GPL 不支持** → `m_SupportsGPL=false`，`CreateVulkanPipeline` 完全走原单片路径，零行为变化。
- **某段库创建失败** → `HE_CORE_WARN` + 本次回退单片路径，失败段不入缓存。
- **fast-link 失败** → `HE_CORE_WARN` + 回退单片创建。
- **fast-linking 不可用** → link 时带 `VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT`（正确但较慢）。
- 所有新增 Vulkan 对象纳入 `DeferredDestructionQueue`，复用现有 3 帧安全销毁机制。

---

## 七、文件变更清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `Engine/RHI/Vulkan/VulkanPipelineLibrary.h` | 新建 | 4 段库缓存 + fast-link 类（含 4 段 key 定义） |
| `Engine/RHI/Vulkan/VulkanPipelineLibrary.cpp` | 新建 | 4 段库查/建 + `LinkPipeline` 实现（~250 行） |
| `Engine/RHI/Vulkan/VulkanDevice_GPL.cpp` | 新建 | `QueryGPLCapabilities()` 能力检测 |
| `Engine/RHI/Vulkan/VulkanDevice.h` | 修改 | `m_SupportsGPL` / `m_SupportsGPLFastLinking` + 访问器 + 限流器队列成员 |
| `Engine/RHI/Vulkan/VulkanDevice.cpp` | 修改 | GPL 扩展启用 + pNext 链 + 限流器 3 方法实现 |
| `Engine/RHI/Vulkan/VulkanPipeline.cpp` | 修改 | `CreateVulkanPipeline` 插入 GPL fast-link 分支 |
| `Engine/RHI/RHI/RHI.h` | 修改 | 限流器 3 个默认空虚方法 |
| `Engine/RHI/CMakeLists.txt` | 修改 | 注册新源文件 |
| `Engine/Render/Pipeline/DeferredPipeline.cpp` | 修改 | `NextFrame()` 限流 drain + 变体演示 Pass + CVar（`static int32_t` 声明，仿 `cvTransientTest`） |

---

## 八、验证

1. 编译通过（RHI + Render + 04.Deferred 全模块零错误）。
2. 运行 04.Deferred（GPL 支持硬件）：
   - 启动日志确认 `Graphics Pipeline Library: 硬件支持已检测（fastLinking=1）`。
   - `set r.GPL.VariantTest 1` 重编译启用 → 日志输出 4 段库编译次数 + 单次 link 耗时 + 总耗时，确认 fast-link 路径生效（非单片）。
3. 限流器生效：N=16 时首个 `NextFrame` 仅创建 3 个 PSO，`GetPendingPSOCreateCount` 逐帧递减至 0。
4. 回归：`r.GPL.VariantTest=0` 时，SSAO/SSAO_Blur/全屏/Compute/Mesh 等现有 PSO 均走原单片路径，无行为变化。
5. 零 Vulkan Validation 错误；`pipeline_cache.bin` 正常生成/加载（与 Phase 1/3 共存无冲突）。
