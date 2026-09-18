# HugEngine 高级图形开发面试题集

> 围绕 HugEngine 引擎架构设计，覆盖 RHI 抽象、RenderGraph、PSO 管线、GPU 内存管理、
> 现代 GPU 特性（Mesh Shader / Ray Tracing / DGC）、延迟渲染、全局光照、后处理等核心技术领域。
> 共 30 题，适合高级图形程序员面试使用。

---

## 一、RHI 抽象层设计（5 题）

### 题 1：RHI 抽象层的核心价值

**题目**：HugEngine 设计了完整的 RHI（Rendering Hardware Interface）抽象层，将 Vulkan 后端细节完全隐藏在 `IRHIDevice` / `IRHITexture` / `IRHICommandList` 等接口之后。请回答：

1. RHI 抽象层与直接调用 Vulkan API 相比，核心价值是什么？
2. HugEngine 如何处理 Vulkan 特有概念（如 VkPipelineCache、VkDescriptorSet）在不破坏抽象的前提下暴露给上层？
3. 如果要新增 D3D12 后端，RHI 层需要做哪些最小改动？

**参考答案要点**：
- ① 核心价值：后端无关性（一套渲染代码多平台运行）、API 复杂度封装（Vulkan 2000+ 行初始化 → 3 行调用）、类型安全转换（`Format` 枚举 → `VkFormat`/`DXGI_FORMAT`）
- ② HugEngine 策略：虚方法默认空实现 + 后端特定 override。如 `CreateTransientTexture()` 在 `IRHIDevice` 默认返回 `nullptr`，仅 `VulkanDevice` override 真实实现；DGC 接口用 `void*` 返回后端句柄
- ③ D3D12 后端：实现所有纯虚方法（CreateTexture、CreatePipelineState 等），新建 `D3D12Device`/`D3D12Texture` 等类，虚方法默认实现的不支持方法无需改动

### 题 2：Vulkan 资源生命周期与延迟销毁

**题目**：HugEngine 使用 `DeferredDestructionQueue` 管理 GPU 资源的生命周期，而非在析构函数中直接调用 `vkDestroy*`。请解释：

1. 为什么不能直接在析构函数中销毁 Vulkan 资源？
2. `DeferredDestructionQueue` 的帧延迟策略（3 帧后销毁）是如果保证安全的？
3. 如果一个纹理在第 N 帧被释放，但第 N+1 帧的 CommandBuffer 仍在引用它，会发生什么？如何防范？

**参考答案要点**：
- ① GPU 异步执行：CPU 析构时 GPU 可能仍在引用该资源，直接销毁会导致 use-after-free
- ② 3 帧延迟 = swapchain 最小 image count(2) + 1 帧安全余量。每帧推进时只销毁队列中最老的一批（≥3 帧前入队）
- ③ 如果 N+1 帧仍引用 → 使用已销毁资源，导致 GPU 崩溃或渲染错误。防范：确保资源仅在确认 GPU 不再使用时入队（配合 FrameGraph 的资源生命周期追踪）

### 题 3：AsyncCompute 队列调度

**题目**：HugEngine 实现了三层 AsyncCompute 能力检测（Tier 0/1/2），并在 RenderGraph 中支持自动异步调度。请回答：

1. Tier 0、Tier 1、Tier 2 分别对应什么硬件能力？
2. RenderGraph 如何自动判断一个 Compute Pass 是否可以异步执行？
3. 跨队列 Barrier（Queue Ownership Transfer）与普通 Pipeline Barrier 有什么不同？

**参考答案要点**：
- ① Tier 0：无独立 Compute 队列（与 Graphics 共享）；Tier 1：独立 Compute 队列族（COMPUTE+TRANSFER）；Tier 2：纯 Compute 硬件引擎（仅 COMPUTE_BIT）
- ② `ScheduleAsyncPasses()` 分析：检查该 Pass 的 writes 是否被后续非 Compute Pass 读取（RAW 依赖），有 → 不可异步，无 → 标记 `asyncSchedule=true`
- ③ Ownership Transfer 传递整个资源的**队列所有权**（VK_PIPELINE_STAGE_TOP_OF_PIPE → BOTTOM）比普通 Barrier 更重，需要 `vkCmdPipelineBarrier` 的 `srcQueueFamilyIndex`/`dstQueueFamilyIndex` 字段；普通 Barrier 只同步同一队列内的 stage/access

### 题 4：Descriptor Set 管理与 Bindless 资源

**题目**：HugEngine 使用 `VK_EXT_descriptor_indexing` 实现 Bindless 资源绑定，每个 Shader 可访问数千个纹理而无需切换 Descriptor Set。请回答：

1. 传统 Descriptor Set 切换的性能开销有多大？为什么 Bindless 能消除这个开销？
2. HugEngine 的 `DescriptorSetLayoutHandle` 和 `DescriptorSetHandle` 设计如何平衡类型安全和灵活性？
3. 如何为 Cubemap 的 6 个面分别创建 ImageView，使得每个面可以独立作为渲染目标？

**参考答案要点**：
- ① 传统切换：每个 Set 绑定涉及 `vkCmdBindDescriptorSets` → 驱动内部状态更新 → 多次调用累积可达数 ms/帧。Bindless 将大量资源打包到一个大 Descriptor Set（运行时索引数组），shader 中用 `NonUniformResourceIndex` 动态索引
- ② `DescriptorSetLayoutHandle` 是不透明整数句柄，内部映射到 `VkDescriptorSetLayout`；绑定通过 `UpdateDescriptorSet` 接口进行强类型检查（区分 Buffer/Texture/Sampler/AS）
- ③ 通过 `CreateTextureMipSampledView(texture, mipLevel, arrayLayer)` 创建逐面 `VkImageView`，`VK_IMAGE_VIEW_TYPE_2D` + `baseArrayLayer=face`

### 题 5：Per-Mip ImageView 与 Hi-Z 构建

**题目**：HugEngine 提供了 `CreateTextureMipStorageView` / `CreateTextureMipSampledView` 接口，允许对纹理的特定 mip level 创建独立的 ImageView。这在 GPU Culling 的 Hi-Z 金字塔构建中至关重要。请解释：

1. 为什么 Hi-Z 构建需要逐 mip 的 Storage ImageView？
2. 如何用 Compute Shader 从深度缓冲逐级构建 Hi-Z 金字塔？写出伪代码或关键步骤。

**参考答案要点**：
- ① 每级 mip 需要作为独立的 UAV 写入目标（`VK_IMAGE_USAGE_STORAGE_BIT`），而非整个纹理的所有 mip。逐 mip ImageView 允许 `levelCount=1`，只暴露单个 mip level 给 Compute Shader
- ② 伪代码：`for mip=1 to maxMip: barrier(level mip-1 → SHADER_READ) + dispatch(8×8 线程采样上一级 2×2 取 min depth) + barrier(level mip → SHADER_READ)`。注意：必须逐级 barrier 而非一次 dispatch 所有 mip

---

## 二、RenderGraph 架构（4 题）

### 题 6：RenderGraph 的 Pass 依赖分析

**题目**：HugEngine 的 `RenderGraph::BuildDependencies()` 使用 RAW/WAW/WAR 三类数据依赖构建 Pass DAG。请回答：

1. RAW、WAW、WAR 分别是什么？为什么都需要考虑？
2. 当前实现使用 O(n²) 的两两比较，对于 100+ Pass 的场景如何优化？
3. 如果两个 Pass 之间没有直接数据依赖，但共享同一个资源的别名内存，如何保证正确性？

**参考答案要点**：
- ① RAW(Read After Write)：后 Pass 读前 Pass 写 → 必须串行；WAW(Write After Write)：两个 Pass 写同一资源 → 需要确定最终值；WAR(Write After Read)：后 Pass 写前 Pass 读 → 防止覆盖
- ② 优化方案：为每个 Resource 维护 lastWriter 和 readers 列表，O(n × resources) → 单次遍历即可建立所有依赖
- ③ 别名资源由 `ApplyAliasing()` 保证生命周期不重叠（区间交集检测），同一 Pool 内的资源永不同时活跃 → 共享内存安全

### 题 7：自动 Barrier 推导

**题目**：HugEngine 的 `RenderGraph::DeriveBarriers()` 自动分析每个 Pass 前后的资源状态转换，生成最小化的 Pipeline Barrier。请回答：

1. 从资源状态机角度，解释 `Undefined → RenderTarget → ShaderResource → RenderTarget` 需要几次 Barrier？
2. 深度资源的 Barrier 推导有什么特殊之处？
3. 如果某个 Pass 对同一资源同时 Read 和 Write，Barrier 推导会如何处理？

**参考答案要点**：
- ① 需要 3 次 Barrier：Undefined→RT（首次转换，可合并到 RenderPass）、RT→SRV、SRV→RT。《Vulkan 最佳实践》建议将多个 Barrier 合并为 `vkCmdPipelineBarrier` 批量提交
- ② 深度资源使用 `AccessToState(access, isDepth=true)` 区分 `DepthStencilRead` vs `DepthStencilWrite`，Layout 转换使用 `VK_IMAGE_LAYOUT_DEPTH_STENCIL_*`。深度写入后必须转为 ReadOnly 才能被 Shader 采样
- ③ RW 模式：`AccessToState(ReadWrite)` 优先选择 Write 状态（RT/DSW），读写在同一 Pass 内通过 subpass self-dependency 或 `VK_ACCESS_MEMORY_READ|WRITE` 保证

### 题 8：资源别名与内存节省

**题目**：HugEngine 的 `RenderGraph::ApplyAliasing()` 使用贪心区间打包算法分析资源生命周期，将非重叠资源共享物理内存。请回答：

1. 给出一个具体例子：3 个 Pass（SSAO → Bloom → DOF）各需要一张全屏纹理，如何通过别名分析节省内存？
2. 贪心算法的时间复杂度是多少？有没有更优的算法？
3. 如果一个资源被标记为 `Imported`（外部导入），为什么不能参与别名分析？

**参考答案要点**：
- ① SSAO(8MB, Pass1) 和 Bloom(8MB, Pass2) 不重叠 → 共享同一块 8MB 内存。DOF(8MB, Pass3) 与前两者都不重叠 → 也可共享同一块。总计 8MB vs 传统 24MB，节省 67%
- ② O(n × pools × used)，最坏 O(n²)。更优：扫描线算法 O(n log n)，按资源 firstUse 排序后线性扫描维护活跃资源集合
- ③ Import 资源是外部创建并跨帧存在的持久纹理（如 GBuffer、SwapChain BackBuffer），生命周期不局限于单帧 RenderGraph，不能与帧内瞬态资源混用同一块内存

### 题 9：Pass 裁剪（Dead Pass Elimination）

**题目**：`RenderGraph::CullDeadPasses()` 移除输出不被任何后续 Pass 消费的 Pass。请回答：

1. 什么情况下会产生 Dead Pass？举两个实际场景。
2. 为什么 Import 资源和 BackBuffer 被标记为"永不被裁剪"？
3. 如果 UI Pass 只在特定条件下启用（如按 F1 打开 Profiler），RenderGraph 应如何处理？

**参考答案要点**：
- ① 场景 1：Shadow Pass 的输出在裁剪后不可见（全在视锥外）；场景 2：调试 Pass 被编译宏关闭但其资源声明仍保留在 FrameGraph 中
- ② Import 资源和 BackBuffer 是外部所有者管理的持久资源，它们的消费者在 RenderGraph 外部（如 SwapChain Present、下一个 Frame 的读取），不能因 RenderGraph 内部无人读取而被错误裁剪
- ③ 将 UI Pass 的 visibility 设为条件性：`if (profilerVisible) rg.AddPass("UI_Profiler", ...)`；CullDeadPasses 会因输出未被消费而自动裁剪（如果 profilerVisible=false 且无人读其输出）

---

## 三、PSO 与 Shader 编译管线（4 题）

### 题 10：VkPipelineCache 持久化机制

**题目**：HugEngine Phase 1 实现了 VkPipelineCache 的磁盘持久化，热启动 PSO 创建从 50ms 降至 ~2ms。请回答：

1. `VkPipelineCache` 内部存储的是什么？为什么它能加速 PSO 创建？
2. HugEngine 在 `VulkanDevice::Initialize()` 中加载 `pipeline_cache.bin`，在 `Shutdown()` 中保存。如果缓存文件损坏或版本不匹配，Vulkan 驱动会如何处理？
3. 在多 GPU 或驱动升级场景下，pipeline_cache.bin 还能复用吗？

**参考答案要点**：
- ① 存储 GPU 驱动编译的中间结果：SPIR-V → GPU ISA 的编译产物。热启动时驱动检测到 cache 命中 → 跳过耗时编译步骤 → 仅做 PSO 链接。《Vulkan 规范》保证 `VkPipelineCache` 对任何合法 PSO 创建请求是透明的（未命中也不报错）
- ② 驱动自动检测校验和：损坏/不匹配的 cache 数据会被静默忽略，回退到空缓存重新编译。HugEngine 无需手动校验
- ③ 多数情况下可以，驱动会内部处理。但跨 GPU 代际（如 NVIDIA→AMD）或驱动大版本升级可能触发 cache 失效。最佳实践：用 GPU deviceID + 驱动版本作为缓存文件名

### 题 11：PSO 哈希与缓存去重

**题目**：HugEngine 使用 FNV-1a 64-bit 哈希对 `PipelineStateDesc` 的所有字段（包括 SPIR-V 二进制内容）计算指纹，在 `VulkanDevice::m_PSOCache` 中实现内存级去重。请回答：

1. 为什么需要对 SPIR-V 的**字节内容**做哈希而不是对指针/文件名？
2. 如果两个 `PipelineStateDesc` 只有 `debugName` 不同，哈希是否应该碰撞？为什么？
3. FNV-1a vs MurmurHash3 vs xxHash：对于 PSO 哈希这个场景哪种最合适？

**参考答案要点**：
- ① SPIR-V 内容决定 GPU 代码逻辑，相同内容的 SPIR-V 可以共享编译产物。指针/文件名不能反映实际内容变化（Shader 热重载后文件内容变了但文件名不变）
- ② 应该碰撞（相同哈希值）——`debugName` 不影响 PSO 的 GPU 行为，两个只在名称上不同的 PSO 应共享同一个 Vulkan 对象以节省内存和创时间
- ③ FNV-1a 足够好：① 输入数据量小（几百 bytes 的状态描述符）→ 速度差异不重要；② 非加密场景 → 碰撞概率 ~1/2^64 可忽略；③ 实现简单无外部依赖

### 题 12：PSO 预热（Precompilation）

**题目**：HugEngine Phase 3 实现了 PSO 预热系统：后台线程将注册的 PSO 变体预先编译到独立的 `VkPipelineCache` 中，完成后通过 `vkMergePipelineCaches` 合并到主缓存。请回答：

1. 为什么后台线程需要**独立的** `VkPipelineCache` 而不是直接用主缓存？
2. `vkMergePipelineCaches` 的内部机制是什么？合并后的缓存大小是两个缓存之和吗？
3. 如果预热线程在渲染已经开始后仍未完成，运行时如何优雅处理？

**参考答案要点**：
- ① Vulkan 规范：`VkPipelineCache` 对象不是线程安全的（`externally synchronized`）。独立缓存避免与主线程的 PSO 创建竞争
- ② 驱动内部将两个缓存的编译产物合并去重。大小通常小于两者之和（因为共享相同的 SPIR-V 数据等）。`vkMergePipelineCaches` 本身是线程安全的
- ③ 运行时检查 `IsReady(hash)`：已预热 → 用主缓存 0 开销创建；未预热 → 降级为同步创建（~2ms）+ 限流（≤3 个/帧）。不会阻塞渲染

### 题 13：PSO 创建限流

**题目**：HugEngine 在运行时效流 PSO 创建（每帧最多 3 个新 PSO），其余的排队到后续帧。请回答：

1. 如果不限流，100+ 个新 PSO 在一帧内创建会造成什么后果？
2. 排队创建的 PSO 在创建完成前，渲染如何继续？会不会出现"紫块"（材质缺失）？
3. 如何决定限流的阈值（3 个/帧）？与硬件能力有何关系？

**参考答案要点**：
- ① 每帧耗时 = 100 × ~2ms = 200ms → 帧率从 60fps 暴跌到 5fps，用户感知为严重卡顿。即使 2ms/个，累积效应不可忽略
- ② 渲染继续用"回退材质"（简单 Unlit Shader）或上一帧的渲染结果，待 PSO 创建完成后下一帧切换到正确材质
- ③ 3 个 × 2ms = 6ms 额外耗时，在 16.6ms 帧预算（60fps）中可接受。阈值应根据 GPU 驱动编译性能动态调整（通过 `GetTimestampPeriod` 测量实际编译耗时）

---

## 四、GPU 内存管理（3 题）

### 题 14：Transient Resource Allocator 原理

**题目**：HugEngine Phase 2 实现了 `TransientResourceAllocator`，使用双缓冲 Heap + Bump Allocator 替代传统的逐纹理 `vkAllocateMemory`。请回答：

1. Bump Allocator 和传统 malloc 的根本区别是什么？为什么 GPU 场景适合 Bump Allocator？
2. 双缓冲（2 Heaps）如何保证 GPU 完成旧帧工作前不覆盖内存？写出时间线分析。
3. `vkBindImageMemory2` 的 offset 是如何确保对齐的？如果对齐错误会有什么后果？

**参考答案要点**：
- ① Bump Allocator 没有 free 操作，只有整体重置。GPU 帧内分配的临时纹理具有统一的生命周期（帧末全部释放）→ 非常适合 bump 模式。无内存碎片
- ② Frame N→Heap0, Frame N+1→Heap1, Frame N+2→Heap0。SwapChain Present 保证 ≥2 帧 GPU 间隔 → Heap0 在 Frame N+2 被重用时已安全。时间线：`|--GPU_N--|--GPU_N+1--|` → N+2 时 N 的 GPU 工作确定完成
- ③ 通过 `VkMemoryRequirements::alignment` 查询 Image 的对齐要求（通常 64KB-256KB），`alignedOffset = (bumpOffset + alignment - 1) & ~(alignment - 1)`。对齐错误 → `vkBindImageMemory2` 返回 `VK_ERROR_INVALID_*` 或 GPU 产生未定义行为

### 题 15：VMA 与显存分配策略

**题目**：HugEngine 使用 VMA（Vulkan Memory Allocator）管理持久 GPU 资源的显存分配。请回答：

1. `VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE` 和 `VMA_MEMORY_USAGE_CPU_ONLY` 分别对应什么分配策略？
2. 为什么 HugEngine 的 Buffer 创建默认加上了 `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`？
3. VMA 如何避免显存碎片化？与直接调用 `vkAllocateMemory` 相比有什么优势？

**参考答案要点**：
- ① `AUTO_PREFER_DEVICE`：优先从 DEVICE_LOCAL 堆分配（VRAM），回退到 HOST_VISIBLE（共享内存）；`CPU_ONLY`：从 HOST_VISIBLE | HOST_COHERENT 分配，适合 Staging Buffer
- ② 用于 Bindless 和 GPU Driven 渲染：Buffer 的 GPU 虚拟地址（`vkGetBufferDeviceAddress`）可以在 Shader 中直接引用，无需额外 Descriptor 绑定
- ③ VMA 内部使用 size-class 分桶 + 大块预分配 + 空闲列表管理。优势：减少 `vkAllocateMemory` 调用次数（Vulkan 规范限制 maxMemoryAllocationCount 通常仅 4096）

### 题 16：VulkanPlacedTexture vs VulkanTexture

**题目**：HugEngine 引入了 `VulkanPlacedTexture` 作为 `VulkanTexture` 的轻量替代，用于瞬态资源。请回答：

1. 两者的内存模型有什么区别？在析构函数中各做了什么？
2. 为什么 `VulkanPlacedTexture` 不通过 VMA 分配？直接使用 `vkBindImageMemory2` 的好处是什么？
3. 如果一个 `VulkanPlacedTexture` 的生命周期超出了其绑定的 Heap（如忘记调用 `AdvanceFrame`），会发生什么？

**参考答案要点**：
- ① `VulkanTexture`：拥有 VkImage + VmaAllocation（独占内存），析构时 `vmaDestroyImage` 同时释放两者。`VulkanPlacedTexture`：拥有 VkImage + VkImageView，不拥有 VkDeviceMemory，析构时只销毁 VkImage 和 VkImageView
- ② VMA 模型是"一个 Image → 一个 Allocation"。瞬态资源需要"多个 Image → 同一个 Heap Memory 的不同 offset"→ 只能用原生 `vkBindImageMemory2`。好处：省去 VMA 内部查找/分配/释放开销
- ③ 下次 `AdvanceFrame()` 切换回该 Heap 时会重置 bump pointer → 新分配可能覆盖旧 Image 的绑定内存 → 旧 Image 读取到垃圾数据或 GPU 崩溃。防范：RenderGraph 在 `Execute()` 开头调用 `AdvanceTransientResources`

---

## 五、延迟渲染管线（3 题）

### 题 17：GBuffer 布局与带宽优化

**题目**：HugEngine 的 DeferredPipeline 使用 6 通道 GBuffer（GB_A/B/C/D/E + Depth）。请回答：

1. 为什么选择 6 个 RT 而非使用更大格式的较少 RT（如 3 个 RGBA32）？
2. GBuffer D（Velocity）使用 `RG16_FLOAT` 格式，为什么只用 16-bit？与 TAA 的 motion vector 精度需求有什么关系？
3. 移动端延迟渲染的主要瓶颈是什么？HugEngine 的架构如何针对移动端优化？

**参考答案要点**：
- ① 6 个 RT 方案：每 RT 格式针对数据特性优化（法线用 RGB10A2、BaseColor 用 RGBA8、Velocity 用 RG16F）→ 总带宽 = Σ 最小化字节数。3 个 RGBA32 方案：大量位数浪费在不需要精度的通道上 → 带宽浪费 30-50%。另一方面，更多 RT 增加 ROP 压力，利弊需要权衡
- ② 运动向量精度要求 ~1/像素，16-bit float 提供约 3 位有效十进制数字（1024×768 分辨率下亚像素精度足够）。32-bit 会浪费 2× 带宽。`RG16_FLOAT` = 4 bytes/pixel，`RGBA32_FLOAT` = 16 bytes/pixel
- ③ 移动端瓶颈：带宽（tile-based GPU 对多 RT 敏感）+ ALU 能力有限。优化：使用 Subpass 在 tile memory 中完成光照计算（减少 HDR 写回），Mobile 模式使用 3 个 RT（Albedo+Normal+Depth）+ 延迟光照在 tile buffer 完成

### 题 18：Clustered Shading 与光照管理

**题目**：HugEngine 实现了 Clustered Shading（`ClusteredShading.h`），将视锥体在 3D 空间中划分为 frustum-aligned 网格。请回答：

1. Clustered Shading 与传统的 Tiled Shading 有什么区别？为什么选择 Cluster 方案？
2. Cluster 的 Z 轴切分为什么不用均匀分块而是对数分块？
3. 深度不连续（如物体边缘）对 Cluster 光照计算有什么影响？如何缓解？

**参考答案要点**：
- ① Tiled：2D 分块（X×Y），每 tile 包含该像素列整个深度范围的所有灯光 → 深度跨度大时大量灯光被包含但实际上被遮挡。Clustered：3D 分块（X×Y×Z），每个 cluster 覆盖更小的深度范围 → 更精确的光源剔除 → 减少无效光照计算
- ② 均匀分块：近处 cluster 覆盖极小的世界空间深度 → 大量 cluster 在近处竞争。对数分块：近处 clusters 更密集（匹配深度缓冲精度分布），远处更稀疏 → 世界空间覆盖更均匀
- ③ 深度不连续导致 cluster 边界处采样的深度值不准确 → 可能漏算或过多包含光源。缓解：使用 2.5D Culling（每个 pixel 独立计算前向深度而非依赖 cluster 边界）+ 保守的 cluster AABB 扩展

### 题 19：GPU Driven 渲染管线

**题目**：HugEngine 支持 GPU Driven 渲染路径（`GBufferMode::GPU`），使用 `GPUCulling` + `GPUScene` + `ExecuteIndirect` 实现 CPU 零剔除的绘制。请回答：

1. `GPUScene::Collect()` 和 `GPUScene::Upload()` 分别在 CPU/GPU 做了什么？
2. GPU Culling 如何通过 Hi-Z 实现遮挡剔除？写出关键步骤。
3. `ExecuteIndirect` vs 传统 `vkCmdDraw` 的核心区别是什么？DGC（Device Generated Commands）在此基础上又做了什么增强？

**参考答案要点**：
- ① `Collect()`：CPU 遍历 SceneGraph 收集可见物体的 Transform/Bounds/MaterialID（不筛选）。`Upload()`：将所有数据打包到 GPU Buffer（ObjectData SSBO），供 GPU 消费
- ② ① 构建 Hi-Z 金字塔（上一帧深度缓冲 → 多级 min-depth）；② Compute Shader 提取物体 AABB 投影到屏幕空间；③ 采样 Hi-Z 对应级别 → 比较物体最近深度 vs Hi-Z 最远深度 → 物体最近深度 > Hi-Z 最远深度 → 完全遮挡 → 剔除
- ③ `vkCmdDraw`：CPU 指定 instanceCount/firstInstance → GPU 直接绘制。`ExecuteIndirect`：GPU 从 Buffer 中读取 draw 参数（由 GPU Culling 写入）+ 间接绘制 → CPU 不知道最终绘制数量。DGC 增强：GPU 同时生成所有 draw commands 到 Device-Generated Buffer → 单次 `vkCmdExecuteGeneratedCommandsEXT` 批量提交

---

## 六、现代 GPU 特性（4 题）

### 题 20：Mesh Shader 与传统 Vertex Shader 的区别

**题目**：HugEngine 支持 `VK_EXT_mesh_shader`，在 `CreateVulkanPipeline` 中有专门的 Mesh Shader 路径。请回答：

1. Mesh Shader 管线与传统 VTG（Vertex-Tessellation-Geometry）管线在数据流上有什么根本区别？
2. 为什么 Mesh Shader 天然适合 GPU Driven 渲染？写出伪代码流程。
3. Task Shader 在 Mesh Shader 管线中扮演什么角色？什么场景适合使用 Task Shader？

**参考答案要点**：
- ① 传统 VTG：固定功能 IA → VS → (Tess) → GS → 固定功能 Primitive Assembly/Raster。Mesh Shader：完全替代 VS+GS+IA，Shader 内部自主生成顶点/图元，输出到 `taskPayload` 或直接光栅化。关键差异：无固定功能 IA，无环形拓扑限制
- ② Mesh Shader 从 SSBO 直接读取 ObjectData + IndirectDraw 参数 → 自行生成顶点 → 输出三角形。无需 CPU 配置 Vertex Buffer / Input Assembly。伪代码：`let obj = g_ObjectData[gl_WorkGroupID.x]; let mesh = g_MeshData[obj.meshID]; for i in 0..meshletCount: output triangles`
- ③ Task Shader 是可选的前置阶段：对大量 meshlet 做粗粒度剔除（如视锥剔除、LOD 选择），将存活 meshlet 分发到多个 Mesh Shader workgroup。适合：开放世界大量物体需要先分组筛选的场景

### 题 21：Ray Tracing 管线与 SBT

**题目**：HugEngine 的 `VulkanRT.cpp` 中实现了完整的 Ray Tracing Pipeline 创建，包括 SBT（Shader Binding Table）构建。请回答：

1. SBT 的作用是什么？它在 GPU 侧如何被索引？
2. `vkGetRayTracingShaderGroupHandlesKHR` 获取的 handle 数据如何映射到具体 Shader？
3. 在 HugEngine 的 `GI_IBL` 和 `GI_RSM` 等 GI 方案之外，Ray Tracing 可以如何增强全局光照？

**参考答案要点**：
- ① SBT：GPU 侧根据 `(rayType, hitGroup, instance, geometry)` 索引 Shader 入口地址的查找表。结构：RayGen 段 + Miss 段 + HitGroup 段 + Callable 段。GPU 通过 `TraceRay` 的 `rayFlags` 和 acceleration structure 层级自动索引
- ② Handle 数据 = GPU 驱动生成的 Shader 入口地址（大小由 `shaderGroupHandleSize` 决定，通常 32 bytes）。构建 SBT 时需要按 `shaderGroupBaseAlignment` 对齐，并正确映射每个 group 的 handle。索引公式：`sbtBase + (groupIndex * handleSizeAligned)`
- ③ RT GI 方案：① RT 环境光遮蔽（RTAO）→ 比 SSAO 更精确；② RT 反射 → 比 SSR 无屏幕空间限制；③ RT 间接漫反射 → 替代 Light Probe/DDGI；④ ReSTIR 路径追踪 → 实时直接/间接光照

### 题 22：Bindless 资源与 GPU Work Graph

**题目**：HugEngine 的 `GPUWorkGraph` 实现了完全在 GPU 侧的渲染任务图调度。请回答：

1. Bindless 资源（`VK_EXT_descriptor_indexing`）与 Work Graph 有什么关系？为什么 Work Graph 需要 Bindless？
2. `NonUniformResourceIndex` 在 SPIR-V 中的作用是什么？为什么需要它？
3. Work Graph 与传统的 ExecuteIndirect 多级调度相比，优势在哪里？

**参考答案要点**：
- ① Work Graph 的每个 Node 需要独立访问不同的纹理/缓冲区 → 传统 Descriptor Set 切换需要 CPU 预知 → 与 GPU 自主调度矛盾。Bindless 将全部资源暴露在一个大 Descriptor Set 中 → GPU 通过索引自由选择
- ② `NonUniformResourceIndex` 告知驱动"这个数组索引在 workgroup 内部不一致"→ 驱动生成正确的非均匀访问代码（某些 GPU 需要额外的 divergence 处理）。缺少此修饰 → 着色器可能产生未定义行为
- ③ Work Graph 优势：GPU 端自适应调度（根据中间结果动态决定下一步执行什么）+ 减少 CPU-GPU 往返 + 支持 Producer-Consumer 模式（Node 的输出自动成为下游 Node 的输入）。ExecuteIndirect 只能做固定流程的多级调度

### 题 23：Timeline Semaphore 与跨队列同步

**题目**：HugEngine 使用 Timeline Semaphore（`VK_SEMAPHORE_TYPE_TIMELINE`）实现 Graphics/Compute 队列间的细粒度同步。请回答：

1. Timeline Semaphore 与传统 Binary Semaphore 的根本区别是什么？
2. 为什么 AsyncCompute 场景需要 Timeline Semaphore 而非 Binary Semaphore？
3. HugEngine 的 `SignalFenceOnQueue` / `WaitFenceOnQueue` 为什么使用空的 `vkQueueSubmit`（无 CommandBuffer）？

**参考答案要点**：
- ① Binary：只有 signaled/unsignaled 两种状态，单次 signal+wait 配对使用。Timeline：单调递增的 64-bit 计数器，支持多对多 signal/wait + CPU 端查询当前值
- ② AsyncCompute 场景中 Graphics 队列和 Compute 队列需要多级同步点（如 Compute→Graphics 多次转移所有权）。Binary Semaphore 一次只能用一对 → 需要多对。Timeline 可以用一个信号量配合不同 timeline 值处理所有同步点
- ③ 空的 `vkQueueSubmit`（零 CommandBuffer、单 Semaphore signal/wait）用于纯同步操作：在队列间插入同步点而不执行任何 GPU 命令。这是 GPU 队列同步的标准做法

---

## 七、全局光照（3 题）

### 题 24：DDGI（Dynamic Diffuse Global Illumination）

**题目**：HugEngine 实现了 DDGI（`GI_DDGI.h`），使用 Probe-based 方法计算动态漫反射全局光照。请回答：

1. DDGI 的 Probe 是如何放置的？如何避免 Probe 穿过墙壁产生漏光？
2. DDGI 中 Probe 的更新频率和精度如何权衡？为什么不需要每帧更新所有 Probe？
3. DDGI 与传统的 Lightmap + Light Probe 方案相比，核心差异是什么？

**参考答案要点**：
- ① Probe 通常按 3D 网格放置（如每 2m 一个）。避免漏光：使用 Probe Relocation（将位置移动到最近的几何体表面）+ Visibility 测试（检查 probe 到采样点之间有无遮挡）+ 背面剔除（backface culling 排除墙后的 probe 贡献）
- ② 每帧更新全部 probe 成本高：N×N×N×R 次光线追踪。优化：每次只更新部分 probes（round-robin 轮询）+ 低帧率更新（每 4 帧一轮）+ 插值平滑过渡。远距离 probes 可更稀疏
- ③ Lightmap：完全静态，预计算（离线），无法响应动态光照/几何变化。DDGI：完全动态，运行时更新，适应动态场景（昼夜循环、可破坏环境）。Lightmap 精度更高但缺乏动态性

### 题 25：SSR/SSGI 的屏幕空间局限与解决方案

**题目**：HugEngine 实现了 SSR（Screen Space Reflections）和 SSGI（Screen Space GI），两者都是屏幕空间技术。请回答：

1. SSR 的主要 artifact 是什么？HugEngine 如何处理反射射线离开屏幕空间的情况？
2. SSGI 如何从屏幕空间深度和颜色近似间接光照？为什么它不需要 Probe 放置？
3. SSGI 与 DDGI 在 HugEngine 中如何配合使用？各自的适用场景是什么？

**参考答案要点**：
- ① 主要 artifact：屏幕边缘截断（反射在屏幕外消失）+ 被遮挡物体的错误反射。处理：Ray Marching 超出屏幕 → 使用 Mipmap 降级的 Cubemap/IBL 作为回退（Fallback）；混合 SSR 和 Fallback 的权重基于射线到屏幕边缘的距离
- ② SSGI 工作原理：对每个像素发射多条半球分布的短射线 → 击中点采样颜色和深度 → 累加为间接光照。它利用当前帧的渲染结果作为近似 → 不需要预计算 Probe。代价：只能照亮屏幕可见表面
- ③ 配合：SSGI 负责高频/近距离间接光照（高精度、屏幕空间限制接受）；DDGI 负责低频/远距离间接光照（全局覆盖、但精度较低）。最终混合两者获得完整 GI 效果

### 题 26：IBL（Image-Based Lighting）与 Cubemap 渲染

**题目**：HugEngine 的 `GI_IBL` 使用预过滤环境贴图（Prefiltered Environment Map）实现基于图像的间接镜面反射。请回答：

1. IBL 的预过滤是如何工作的？为什么不同粗糙度需要不同 mip level？
2. HugEngine 的 `CreateTextureMipSampledView(texture, mip, arrayLayer)` 在 Cubemap 渲染中发挥什么作用？
3. BRDF Integration Map（LUT）的作用是什么？为什么可以预计算成 2D 纹理？

**参考答案要点**：
- ① 预过滤：对原始 Cubemap 的每个 mip level 用不同粗糙度的 GGX 核卷积 → mip0=镜面、mip5=粗糙面。采样时：`textureLod(envMap, reflectDir, roughness * maxMip)`。粗糙度→mip 的映射基于等效的滤波核大小
- ② 逐面渲染 Cubemap 时，需要将 6 个面渲染到 `VK_IMAGE_VIEW_TYPE_2D` 的对应 array layer → `CreateTextureMipSampledView(cubeTex, 0, faceIndex)` 创建 `baseArrayLayer=face, layerCount=1` 的逐面视图
- ③ BRDF LUT 将 GGX BRDF 的积分拆分为两项：`F0 * scale + bias` → 预计算 `(NdotV, roughness) → (scale, bias)` 存储为 `RG16_FLOAT` 2D 纹理。可以预计算因为 GGX BRDF 只依赖这两个参数

---

## 八、后处理与抗锯齿（2 题）

### 题 27：TAA（Temporal Anti-Aliasing）的时序混合

**题目**：HugEngine 实现了 TAA（`AA_TAA.h`），使用历史帧数据与当前帧混合来消除锯齿。请回答：

1. TAA 为什么需要 Motion Vector？Motion Vector 是如何生成的？
2. TAA 的 ghosting（残影）问题是如何产生的？HugEngine 如何缓解？
3. TAA vs FXAA vs SMAA vs MSAA：四者的优缺点和在 HugEngine 中的适用场景。

**参考答案要点**：
- ① TAA 需要将历史帧像素重投影到当前帧的对应位置 → 需要知道每个像素从上一帧移动的距离 = Motion Vector。生成：GBuffer D 中存储 `CurrentScreenPos - PreviousScreenPos`，基于 `m_PrevViewProj` 矩阵重投影世界空间位置
- ② Ghosting 原因：Motion Vector 不准确（阴影变化、光照变化、透明物体）→ 历史数据与当前帧不匹配 → 混合后产生拖影。缓解：Color Clamping（将历史颜色 clamp 到当前帧邻域 min/max 范围内）+ 动态混合权重（检测 disocclusion → 降低历史权重）
- ③ TAA：最佳静态画质，需 Motion Vector，有 ghosting → HugEngine 默认方案。FXAA：后处理单帧分析，轻量但模糊 → 移动端/低配。SMAA：形态学边缘检测+混合，质量介于 FXAA/TAA → 中配。MSAA：硬件多重采样，完美几何边缘但无法抗着色锯齿 → 前向渲染/移动端

### 题 28：AutoExposure 的亮度直方图

**题目**：HugEngine 的 `AutoExposurePass` 基于场景亮度自动调整曝光。请回答：

1. 为什么 AutoExposure 需要基于**上一帧**的亮度数据而非当前帧？
2. 亮度直方图的生成流程是什么？如何从直方图计算曝光值？
3. 曝光值的平滑过渡是如何实现的？如果场景突然变亮（如走出隧道），如何平衡响应速度和平滑度？

**参考答案要点**：
- ① 当前帧的亮度需等渲染完成后才能统计 → 造成循环依赖。使用上一帧避免延迟：帧 N 渲染 → 统计亮度 → 用于帧 N+1 的曝光。单帧延迟人眼几乎无感知
- ② 流程：① Compute Shader 生成亮度直方图（256 bins，log2 分布）；② 计算场景平均亮度（histogram 加权）；③ `EV = log2(averageLuminance * kKeyValue / kCalibrationConstant)`；④ 应用曝光补偿（用户设置）
- ③ 平滑：`newExposure = lerp(prevExposure, targetExposure, adaptSpeed * deltaTime)`。adaptSpeed 区分明暗适应：亮→暗慢（0.5-1.0/s），暗→亮快（2.0-4.0/s）→ 模拟人眼的非对称适应特性

---

## 九、性能分析与调试（2 题）

### 题 29：GPU Profiler 与 RenderDoc 集成

**题目**：HugEngine 内置了 GPU Profiler（`ProfilerManager.h`），使用 `vkCmdWriteTimestamp` 测量每个 Pass 的 GPU 耗时。请回答：

1. `vkCmdWriteTimestamp` 的精度有多高？与 CPU 端 `QueryPerformanceCounter` 的精度对比如何？
2. 为什么 GPU 时间戳需要在 command buffer 执行完毕后才能读取（Readback）？HugEngine 如何处理这个异步延迟？
3. RenderDoc 帧捕获时，Debug Label（`vkCmdBeginDebugUtilsLabelEXT`）的作用是什么？HugEngine 如何集成这些标签？

**参考答案要点**：
- ① GPU 时间戳精度 = `timestampPeriod`（通常 ~40-80ns 或 20-50MHz），由 `vkGetPhysicalDeviceProperties::limits::timestampPeriod` 查询。CPU `QPC` 精度 ~100ns。GPU 时间戳量化误差 = timestampPeriod
- ② GPU 时间戳在 command buffer 执行到对应位置时写入 → CPU 读取时可能尚未执行 → 读到未定义值。HugEngine 使用 N 帧延迟读取：`m_QueryPool` 循环使用，帧 N 的时间戳在帧 N+3 读取（保证 GPU 已完成）
- ③ Debug Label 在 RenderDoc 的 Event Browser 中显示为命名区域 → 方便定位每个 Pass 的 Draw Call。HugEngine：`cmdList->BeginDebugLabel(pass->name)` → 内部调用 `vkCmdBeginDebugUtilsLabelEXT`。每个 Pass 都包裹 Label，RenderDoc 中清晰可见整个 RenderGraph 结构

### 题 30：Shader 热重载系统

**题目**：HugEngine 实现了 Shader 热重载（`ShaderHotReload.h`），开发者修改 Slang Shader 源码后无需重启程序即可看到效果。请回答：

1. Slang 编译器在热重载流程中扮演什么角色？与直接使用 `glslc` 编译有什么不同？
2. 热重载触发后，PSO 缓存中的旧 PSO 如何处理？如何保证不影响正在渲染的帧？
3. 如果 Shader 的 Descriptor Set 布局发生了变化（如新增了一个 binding），热重载能否处理？为什么？

**参考答案要点**：
- ① Slang：支持 HLSL/GLSL/CUDA 多目标输出 → 编译为 SPIR-V。热重载：文件系统监控 → 检测变更 → 调用 `slangc` 重新编译 → 生成新的 `.spv.h` header。相比 `glslc`，Slang 支持 Parameter Block（自动生成 descriptor set layout）+ 跨平台
- ② 旧 PSO 通过 `DeferredDestructionQueue` 延迟销毁（3 帧后安全释放）。新 PSO 创建时使用新的 SPIR-V 内容 → 哈希不同 → 新 entry 插入 `m_PSOCache`。渲染中的帧继续使用旧 PSO 引用（shared_ptr 保证生命周期）
- ③ **不能**处理。Descriptor Set Layout 变更需要重新创建 `VkDescriptorSetLayout` + `VkPipelineLayout` → 影响整个资源绑定→ 需要重新初始化渲染管线。Shader 热重载仅适用于**代码逻辑变更**（算法调整、参数调优），不适用于接口变更

---

## 附录：题目分类与难度分布

| 分类 | 题号 | 题目数 | 难度 |
|------|------|--------|------|
| RHI 抽象层设计 | 1-5 | 5 | ★★★☆ |
| RenderGraph 架构 | 6-9 | 4 | ★★★★ |
| PSO 与 Shader 编译 | 10-13 | 4 | ★★★☆ |
| GPU 内存管理 | 14-16 | 3 | ★★★★ |
| 延迟渲染管线 | 17-19 | 3 | ★★★☆ |
| 现代 GPU 特性 | 20-23 | 4 | ★★★★★ |
| 全局光照 | 24-26 | 3 | ★★★★ |
| 后处理与抗锯齿 | 27-28 | 2 | ★★☆☆ |
| 性能分析与调试 | 29-30 | 2 | ★★☆☆ |

**难度说明**：★ = 基础概念，★★★★★ = 需要深入理解 Vulkan 规范 + HugEngine 实现细节

---

> 题目围绕 HugEngine v0.1.0 实际代码架构设计，覆盖 `Engine/RHI/`、`Engine/Render/` 核心模块。
> 建议面试时按分类选择题目，根据候选人回答深浅灵活评分。
