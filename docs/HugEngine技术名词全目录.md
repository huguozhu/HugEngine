# HugEngine 技术名词全目录

> 2026-07-22 | 完整引擎技术概念索引，共 102 个技术名词
> 按系统分层：核心引擎 → RHI → RenderGraph → 渲染管线 → 后处理 → GI → GPU驱动 → 光线追踪 → 工具链

---

## 一、核心引擎系统

### 1. 引擎启动（Engine Bootstrap）
- **类别**: 核心引擎
- **原理**: `Engine` 类按严格顺序编排：日志（spdlog）→ JobSystem（Taskflow）→ 窗口（GLFW）。`EngineConfig` 允许自定义窗口尺寸、垂直同步、线程数和验证层。关闭以相反顺序执行。
- **解决问题**: 统一的引擎生命周期管理，所有子系统按依赖顺序初始化。

### 2. 类型系统（Fundamental Types）
- **类别**: 核心引擎
- **原理**: 引擎范围的固定宽度整数类型别名（`i8`～`i64`、`u8`～`u64`）、字符串类型（`String`/`StringView`）、智能指针（`UniquePtr`/`SharedPtr`）。`HE_DECLARE_NON_COPYABLE`/`HE_DECLARE_NON_MOVABLE` 宏强制执行 RAII。
- **解决问题**: 跨平台的确定性类型大小，避免标准库类型差异导致的 ABI 问题。

### 3. 断言系统（Assertion System）
- **类别**: 核心引擎
- **原理**: `HE_ASSERT` 在 Debug 构建中提供带文件名、行号的致命断言失败。`HE_UNREACHABLE()` 调用编译器内置的不可达标记。Release 构建中均为空操作。
- **解决问题**: 开发期快速定位逻辑错误，不影响发布性能。

### 4. 日志系统（Logging via spdlog）
- **类别**: 核心引擎/第三方
- **原理**: 基于 spdlog 的薄封装，维护 Core 和 Client 两组独立 logger。支持 7 级严重度（Trace→Critical），彩色控制台输出。
- **解决问题**: 高性能异步日志，Release 构建可裁剪低级别日志。

### 5. 平台检测（Platform Detection）
- **类别**: 核心引擎
- **原理**: 预处理器宏检测 Windows/macOS/iOS/Linux + MSVC/Clang/GCC + x64/ARM64。提供 `HE_FORCE_INLINE`、`HE_ALIGN(n)` 等编译器属性宏。
- **解决问题**: 跨平台编译的预处理抽象，避免 `#ifdef` 散落各处。

---

## 二、JobSystem（作业系统）

### 6. JobSystem
- **类别**: 核心引擎/第三方
- **原理**: Taskflow（`tf::Taskflow` + `tf::Executor`）的薄封装。提供 `Submit`（发射即忘）、`ParallelFor`（范围并行）、`ParallelForChunked`（分块并行）、`ParallelInvoke`（并行调用）、`Async`（异步任务）。`ParallelForEach` 在超过 1024 元素时自动并行化。
- **解决问题**: CPU 多核并行计算，用于场景遍历、视锥剔除等。

---

## 三、窗口系统

### 7. Window
- **类别**: 核心引擎
- **原理**: 封装 GLFW 窗口创建，使用 `GLFW_NO_API`（无 OpenGL 上下文）。暴露原生句柄（`glfwGetWin32Window`）给 RHI 层。
- **解决问题**: 跨平台窗口创建，与 Vulkan Surface 集成。

---

## 四、数学库

### 8. GLM 类型别名
- **类别**: 核心引擎/第三方
- **原理**: `float2`/`float3`/`float4`、`float4x4`、`quat` 等均为 GLM 类型的引擎别名。配置 Vulkan 深度范围 [0,1]（`GLM_FORCE_DEPTH_ZERO_TO_ONE`）、弧度角度。
- **解决问题**: 统一的数学类型命名约定，与着色器端对齐。

### 9. 几何图元（Geometry Primitives）
- **类别**: 核心引擎
- **原理**: AABB（轴对齐包围盒，支持 Transform/Expand/Corners）、Frustum（Gribb-Hartmann 6 平面提取，支持点/球/AABB 相交测试）、Ray（支持 AABB/球/三角形相交）、Sphere。
- **解决问题**: 渲染管线和物理系统的基础碰撞/可见性测试。

---

## 五、内存管理

### 10. IAllocator / MallocAllocator
- **类别**: 核心引擎
- **原理**: `IAllocator` 抽象基类，提供 `Allocate()`/`Deallocate()` + 对齐支持。`MallocAllocator` 基于 `std::aligned_alloc` 实现。`HE_NEW`/`HE_DELETE` 宏通过引擎默认分配器路由。
- **解决问题**: 统一内存分配入口，为未来内存追踪/调试可视化打基础。

---

## 六、容器

### 11. 容器类型别名
- **类别**: 核心引擎
- **原理**: `TArray<T>`（`std::vector`）、`TMap<K,V>`（`std::unordered_map`）、`THashSet<T>`、`TDeque<T>`、`TFixedArray<T,N>`（`std::array`）。
- **解决问题**: 统一容器命名，方便后续替换 STL 实现。

### 12. TInlineVec（小型内联向量）
- **类别**: 核心引擎
- **原理**: 小型向量优化容器——元素数 ≤ InlineCapacity 时使用栈分配，超出后溢出到堆 `std::vector`。数据访问透明路由。
- **解决问题**: 避免小集合的堆分配开销。

---

## 七、反射系统

### 13. ReflectionAPI / TypeRegistry
- **类别**: 核心引擎
- **原理**: `PropertyInfo`（成员名、偏移量、大小、类型名、标志）+ `ClassInfo`（类名、大小、FNV-1a 哈希、工厂函数、属性列表、父类指针）。`TypeRegistry` 全局单例按哈希索引。
- **解决问题**: 运行时反射，支撑序列化、编辑器属性面板、CVar 绑定。

### 14. 反射宏系统（Reflection Macros）
- **类别**: 核心引擎
- **原理**: `HE_CLASS()`/`HE_COMPONENT()` 宏生成 `StaticClass()`/`GetClass()`。`.cpp` 中使用 `HE_BEGIN_REGISTER` → `HE_REGISTER_PROPERTY` + `HE_ATTR_*` 注解 → `HE_END_REGISTER` 注册。
- **解决问题**: 声明式反射注册，为 C++26 `consteval` 反射迁移预留路径。

### 15. 属性系统（Attribute System）
- **类别**: 核心引擎
- **原理**: 标准化元数据键：`Category`、`DisplayName`、`Tooltip`、`Range(min,max)`、`Clamp`、`AssetPicker(.gltf)`、`ColorWidget`、`Unit("cm")`、`EditCondition` 等。
- **解决问题**: 编辑器 UI 自动生成所需的元数据，属性面板无需手写。

---

## 八、序列化系统

### 16. IArchive / BinaryArchive
- **类别**: 核心引擎
- **原理**: `IArchive` 定义读写接口，支持基础类型、String、float3/float4/quat、Object/Array。`BinaryArchive` 实现紧凑二进制格式。
- **解决问题**: 场景持久化（`.hescene` 格式）、未来支持 JSON/XML 存档。

---

## 九、RHI（渲染硬件接口）层

### 17. IRHIDevice — 设备抽象
- **类别**: RHI 核心
- **原理**: 纯虚工厂类，是所有 GPU 资源的入口。提供 SwapChain、CommandList、Buffer、Texture、Sampler、PipelineState、AccelerationStructure、RTPipeline、DescriptorSet、QueryPool、Fence 的工厂方法。全局单例 `g_Device`。
- **解决问题**: 将引擎所有模块与具体图形 API 解耦。不包含任何 Vulkan/D3D12 头文件。

### 18. Backend 枚举
- **类别**: RHI 核心
- **原理**: `Vulkan`（已实现）、`D3D12`、`Metal`、`WebGPU`。`CreateDevice(Backend)` 工厂分发。
- **解决问题**: 多后端架构，编译时或运行时选择 API。

### 19. QueueType 枚举
- **类别**: RHI 核心
- **原理**: `Graphics`、`Compute`、`Copy` 三种队列类型。用于 AsyncCompute 卸载和跨队列同步。
- **解决问题**: GPU 硬件并行——图形和计算在独立队列上并发执行。

### 20. DeviceCaps
- **类别**: RHI 核心
- **原理**: 结构体报告 GPU 硬件支持：光线追踪（AS/RT 属性）、Mesh Shader（工作组调用/顶点/图元属性）、DGC、VRS、SER、OMM、CooperativeVectors 等。
- **解决问题**: 高层代码按 GPU 能力进行功能降级和自动缩放。

### 21. Format 枚举
- **类别**: RHI 核心
- **原理**: 约 30 种跨平台格式：R8/RG8/RGBA8 UNORM/SRGB、BGRA8、RGBA16/32_FLOAT、D16/D32/D24S8/D32S8、BC1-7 压缩。
- **解决问题**: 与 API 无关的像素格式标识符，后端映射为 Vulkan/D3D12 本地格式。

### 22. PipelineStateDesc — PSO 描述符
- **类别**: RHI 核心
- **原理**: 完整描述一个图形或计算管线：着色器字节码指针（vertex/pixel/compute/mesh/amplification）、VertexInputLayout、光栅化状态（剔除/面朝向/填充/深度）、kMaxColorAttachments 颜色格式+混合、深度格式、LoadOp、MSAA 采样数、PushConstant 范围、DescriptorSetLayout 数组、调试名。
- **解决问题**: 单一结构体完整描述 PSO，支持唯一哈希和在缓存中去重。

### 23. HashPipelineStateDesc（FNV-1a 64-bit）
- **类别**: RHI 核心
- **原理**: 对 PipelineStateDesc 的所有字段进行 FNV-1a 64 位哈希，包括 SPIR-V 字节码的完整内容（非指针）——任何着色器改动都会触发缓存 miss。
- **解决问题**: 稳定的 64 位 PSO 指纹，用于缓存去重和热重载检测。

### 24. PSO 缓存（运行时内存内）
- **类别**: RHI 核心
- **原理**: `unordered_map<uint64_t, PSOCacheEntryInternal>`，键为 PSO 哈希。存储 VkPipeline + VkPipelineLayout + VkRenderPass + 共享引用计数 + lastUsedFrame。查询命中后返回共享底层 Vulkan 对象的 `VulkanPipelineState`。三路径统一：Compute/Mesh/Graphics。
- **解决问题**: 消除重复 PSO 编译，同配置管线只创建一次。

### 25. VulkanPipelineState — 双所有权模式
- **类别**: RHI 核心
- **原理**: 自拥有模式（直接销毁 VkPipeline）和缓存模式（shared_ptr 引用计数 → use_count==2 时入队 DeferredDestructionQueue，3 帧后安全销毁）。
- **解决问题**: 缓存 PSO 的安全生命周期——最后一个引用释放后不立即销毁，等 GPU 用完。

### 26. VkPipelineCache 持久化
- **类别**: RHI 核心/磁盘缓存
- **原理**: `LoadPipelineCache()` 在 Init 时从 `pipeline_cache.bin` 读取 → 创建 VkPipelineCache。`SavePipelineCache()` 在 Shutdown 时调用 `vkGetPipelineCacheData` → 回写磁盘。
- **解决问题**: 跨运行保存 GPU 驱动的管线编译数据。热启动 50ms→2ms（25× 加速）。

### 27. IRHICommandList — 命令录制抽象
- **类别**: RHI 核心
- **原理**: 最大接口（约 40 个纯虚方法）：Begin/End、BeginRenderPass/OffscreenPass/MRT、SetPipeline/VertexBuffer/IndexBuffer、Draw/DrawIndexed/Indirect、DrawMeshTasks/Inirect、Dispatch/Indirect、PipelineBarrier、CopyBuffer/Texture、QueueOwnershipTransfer、BeginDebugLabel/EndDebugLabel、ExecuteGeneratedCommands (DGC)、WriteTimestamp 等。
- **解决问题**: Vulkan 命令录制和 D3D12 命令列表/分配器的完整抽象。

### 28. 屏障系统（PipelineBarrier）
- **类别**: RHI 核心/同步
- **原理**: `ResourceState`（Undefined/RenderTarget/DepthStencilWrite/ShaderResource/UnorderedAccess/Present）→ `VkImageLayout`。`PipelineStage`（TopOfPipe/VertexShader/FragmentShader/ComputeShader/Transfer）→ `VkPipelineStageFlags`。`AccessFlags` 控制内存访问类型。
- **解决问题**: 声明式屏障——调用方只需描述资源状态转换，RHI 自动映射到 Vulkan 屏障。

### 29. 描述符集管理
- **类别**: RHI 核心
- **原理**: 九种描述符类型（Uniform/StorageBuffer、CombinedImageSampler、StorageImage、SampledImage、Sampler、AccelerationStructure）。支持 bindless 数组（变长描述符计数 + `UPDATE_AFTER_BIND`）。三个描述符集：Set=0（逐帧：相机/灯光）、Set=1（逐材质：bindless）、Set=2（bindless：TLAS）。
- **解决问题**: 抽象 Vulkan 描述符布局和 D3D12 根签名，支持无绑定纹理。

### 30. VulkanDevice
- **类别**: RHI/Vulkan 后端
- **原理**: 实现全部 IRHIDevice。管理：VkInstance → VkPhysicalDevice → VkDevice（含 VMA） → VkSurface → 队列族 → 命令池 → Timeline Semaphore 池 → 描述符池 → VkPipelineCache。初始化 12 步序列，关闭 15 步序列。
- **解决问题**: 完整的生产级 Vulkan 设备封装，编排所有生命周期。

### 31. VulkanCommandList — 三重缓冲录制
- **类别**: RHI/Vulkan 后端
- **原理**: 3 个 VkCommandPool + 3 个 VkCommandBuffer + 3 个 VkFence。`Begin()` 等当前帧 fence → 推进帧计数器 + 延迟销毁队列 → 重置命令池。跟踪绑定状态（Pipeline/Layout/RenderPass/VB/IB/BindPoint）以高效重绑定。支持 Secondary Command Buffer 并行录制。SwapChain framebuffer 懒构建。
- **解决问题**: 无 GPU-CPU 同步停顿的命令录制，支持并行录制 + 调试标签。

### 32. VulkanRTDispatch — 光线追踪函数指针表
- **类别**: RHI/Vulkan 后端/光线追踪
- **原理**: 8 个 RT 扩展函数指针：vkCreateAccelerationStructureKHR、vkDestroyAS、vkGetASBuildSizes、vkCmdBuildAS、vkGetASDeviceAddress、vkCreateRTPipelines、vkGetRTShaderGroupHandles、vkCmdTraceRays。通过 vkGetDeviceProcAddr 动态加载。
- **解决问题**: 集中加载 Vulkan RT 扩展函数，避免每个调用点查找。

### 33. VulkanDGC — Device Generated Commands
- **类别**: RHI/Vulkan 后端/DGC
- **原理**: 封装 VK_EXT_device_generated_commands。管理：VkIndirectCommandsLayoutEXT（DRAW_INDEXED 令牌格式）、VkIndirectExecutionSetEXT（关联初始管线）、预处理缓冲区（vkGetGeneratedCommandsMemoryRequirementsEXT）。IsSupported() 查询硬件支持。
- **解决问题**: GPU 自主生成绘制命令，消除 CPU readback 可见性结果。

### 34. Timeline Semaphore 管理
- **类别**: RHI/Vulkan 后端/同步
- **原理**: `RHIFenceHandle`（不透明 u64）封装 `VkSemaphore`（VK_SEMAPHORE_TYPE_TIMELINE）。`CreateFence()` 分配 → `SignalFenceOnQueue`/`WaitFenceOnQueue` 通过 VkTimelineSemaphoreSubmitInfo 提交 → CPU 端 `WaitForFence` 用 vkWaitSemaphores。
- **解决问题**: 跨 GPU 队列族的 GPU 端同步（Graphics↔AsyncCompute），单一原语支持信号和等待。

### 35. 调试对象命名（VK_EXT_debug_utils）
- **类别**: RHI/Vulkan 后端/调试
- **原理**: 动态加载 vkSetDebugUtilsObjectNameEXT（对象命名）+ vkCmdBeginDebugUtilsLabelEXT/vkCmdEndDebugUtilsLabelEXT/vkCmdInsertDebugUtilsLabelEXT（命令标签）。`SetResourceDebugName()` 覆盖 Buffer/Texture/PipelineState/Sampler/AS。
- **解决问题**: RenderDoc/NSight/PIX 中 GPU 资源和 RenderPass 可见名称。

---

## 十、Transient Resource Allocator（瞬态资源分配器）

### 36. TransientResourceAllocator
- **类别**: 内存管理
- **原理**: 双缓冲堆池（2×128MB VkDeviceMemory），帧内 Bump Allocator（bumpOffset 对齐递增）。`AllocateImage()`：构建 VkImage 缓存键 → 查找/创建 VkImage → 查询内存需求 → bump 分配 offset → vkBindImageMemory2。`AdvanceFrame()` 切换堆并重置 bump 指针。安全由双缓冲保证：堆 A 帧 N，堆 B 帧 N+1，帧 N+2 重用时 GPU 已完成（FIFO 呈现 ≥2 帧间隔）。
- **解决问题**: 帧内临时纹理的 O(1) 零碎片 GPU 内存子分配。消除每帧 vkAllocateMemory/vkFreeMemory + vkCreateImage/vkDestroyImage。

### 37. ImageCacheKey — VkImage 对象复用
- **类别**: 内存管理
- **原理**: 捕获 VkImage 不变参数（imageType/format/width/height/depth/mipLevels/arrayLayers/samples/usage/flags）并 FNV-1a 哈希。分配时从缓存弹出；帧切换时放回；关闭时批量销毁。
- **解决问题**: 防止同规格 VkImage 的重复创建/销毁，显著降低 VkImage 生命周期开销。

---

## 十一、PSO 预热管理器

### 38. PSOPrecompileManager
- **类别**: 管线管理/预编译
- **原理**: 后台工作线程从主 VkPipelineCache 派生独立 worker cache → 逐项创建 Compute/Graphics PSO（vkCreateShaderModule → vkCreatePipelineLayout → vkCreateRenderPass → vkCreate*Pipelines），编译完成后立即销毁临时 VkPipeline（编译数据保留在 cache 中）→ `vkMergePipelineCaches(main, worker)` 合并。主线程通过 `QueuePSO(desc)` 注册 → `StartPrecompile()` 启动 → `GetProgress()` 查询进度（原子计数器，无锁）。
- **解决问题**: 后台编译 PSO 变体预热驱动缓存。合并后主线程 CreatePipelineState 仅需约 2ms（vs 冷启动 50ms）。

---

## 十二、DeferredDestructionQueue（延迟销毁队列）

### 39. DeferredDestructionQueue
- **类别**: 内存管理/生命周期
- **原理**: 3 帧槽位的回调队列。`Enqueue(deleter)` 写入当前槽；`Advance()` 轮换槽位并执行最旧槽的析构函数（GPU 已安全完成）。`FlushAll()` 仅关闭时（vkDeviceWaitIdle 后）立即全部执行。
- **解决问题**: GPU 资源的安全删除——入队后 GPU 使用完前不销毁。统一了所有散落的 ad-hoc 延迟销毁模式。

---

## 十三、RenderGraph（渲染图）

### 40. RenderGraph
- **类别**: 帧图编排
- **原理**: 帧级渲染资源编排器。资源通过 `CreateTexture()`/`CreateBuffer()` 创建或 `ImportTexture()` 导入。Pass 通过 `AddPass()` 添加，声明 reads/writes。编译五阶段：(1) BuildDependencies（RAW/WAW/WAR）、(2) TopologicalSort、(3) DeriveBarriers（自动布局转换）、(4) CullDeadPasses（未消费输出的 Pass 裁剪）、(5) ApplyAliasing（贪心区间打包→内存池分配）。Execute() 实例化纹理/缓冲区，应用屏障，依次运行。
- **解决问题**: 声明式 Pass 编排——Pass 只声明读写，图自动计算顺序、屏障和内存复用。

### 41. ResourceHandle
- **类别**: 帧图核心
- **原理**: `u32` 不透明句柄（无效 = ~0u），标识图内任意纹理/缓冲区资源。Pass 输入/输出全部引用 Handle，从来不用裸指针。
- **解决问题**: 资源生命周期管理解耦——图可以重排、裁剪、别名化资源，Pass 不知彼此。

### 42. 资源别名（ApplyAliasing）
- **类别**: 帧图内存优化
- **原理**: 贪婪区间算法：计算每个资源的生命周期（首次和末次 Pass 索引）。生命周期不重叠的资源放入共享内存池（按 offset），纹理大小按 `width*height*8` 估算。
- **解决问题**: 时域不重叠资源共享同一物理内存，降低 GPU 内存占用。

### 43. 死 Pass 裁剪（CullDeadPasses）
- **类别**: 帧图优化
- **原理**: 标记被下游 Pass 消费的资源 + 导入纹理 + BackBuffer 为"已消费"。输出资源无消费者的 Pass 从执行顺序中移除。
- **解决问题**: 自动消除输出无人使用的 Pass（如禁用的后处理效果）。

### 44. AsyncCompute 调度
- **类别**: 帧图 AsyncCompute
- **原理**: `ExecuteWithAsyncCompute()` 将 Compute Pass（GPU_Cull、SSAO、DDGI、AutoExposure）分离到独立 Compute 队列。构建两个命令列表，Timeline Semaphore 跨队列同步。跨队列所有权转移（QueueOwnershipTransfer）自动插入。
- **解决问题**: 计算与图形工作硬件并行，减少帧时间。

### 45. Barrier 推导（DeriveBarriers）
- **类别**: 帧图核心
- **原理**: 拓扑排序后遍历 Pass。每资源访问比较当前状态与所需状态（AccessToState：Read→ShaderResource，Write→RenderTarget），不同则记录 BarrierRecord（srcState/dstState/srcStage/dstStage）。
- **解决问题**: 自动屏障推导——Pass 作者不关心资源转换，图从声明推导。

---

## 十四、渲染管线

### 46. DeferredPipeline — 延迟渲染
- **类别**: 渲染管线
- **原理**: GBuffer 5×MRT：Slot 0=Albedo+Metallic(RGBA16_FLOAT)、Slot 1=Normal+Roughness(RGBA16_FLOAT)、Slot 2=Emissive+AO(RGBA16_FLOAT)、Slot 3=Velocity(RG16_FLOAT)、Slot 4=WorldPos(RGBA16_FLOAT)。Deferred Lighting 全屏三角形采样全部 GBuffer + CSM 阴影 + IBL + RSM + SSGI + SSAO + SSR + DDGI + Clustered Shading，单 Pass 完成全部光照。
- **解决问题**: 单遍几何 + 全屏光照，减少带宽 vs 多 Pass 光照。

### 47. ForwardPipeline — 前向渲染
- **类别**: 渲染管线
- **原理**: 单 PBR PSO（PBR.vert + PBR.frag），支持 Forward+（Clustered Shading）、多线程命令录制（辅助命令列表并行录制）、ExecuteIndirect（GPU 驱动绘制）、光线追踪（RTPass: BLAS/TLAS/SBT）。HDR 输出 `RGBA16_FLOAT`。
- **解决问题**: 前向 PBR 渲染路径，适合半透明材质和简单场景。

### 48. GBuffer 5×MRT 布局
- **类别**: 延迟着色
- **原理**: 5 张渲染目标 + 深度，单次几何 Pass 存储全部表面属性。GBuffer PSO 使用 bindless 描述符集（set=0: GPUObjectData SSBO + Texture[4096] + Sampler[4096]）。
- **解决问题**: 光照 Pass 无需重新访问几何——全屏三角形直接读取 GBuffer。

### 49. GBufferMode（CPU vs GPU 驱动）
- **类别**: 延迟着色
- **原理**: `CPU` 模式逐对象 DrawIndexed（push constant objectIndex）。`GPU` 模式用 MeshBatcher 合并 VB/IB + DrawIndexedIndirect（GPU 剔除输出驱动）。支持 DGC 模式（vkCmdExecuteGeneratedCommandsEXT）。
- **解决问题**: 从传统 CPU 驱动到完全 GPU 驱动渲染的平滑迁移路径。

### 50. DGC（Device Generated Commands）
- **类别**: GPU 驱动渲染
- **原理**: `r.DGC.Enable` CVar 控制。GPU 剔除输出 `DGCDrawToken` 数组 → 作为 DGC 序列传给 vkCmdExecuteGeneratedCommandsEXT。GPU 自主生成绘制命令，无需 CPU readback。
- **解决问题**: 绘制命令生成完全 GPU 化，消除 CPU 可见性回读瓶颈。

### 51. 三缓冲（Triple Buffering）
- **类别**: 基础设施
- **原理**: 3 组 SSBO（Light/Object/Shadow/ShadowObj Buffer），`m_CurrentFrameSlot = (slot+1)%MAX_FRAMES_IN_FLIGHT` 轮换。
- **解决问题**: 消除 GPU-CPU 同步停顿——CPU 准备帧 N+2 时 GPU 仍在消费帧 N 和 N+1。

---

## 十五、后处理链路

### 52. ToneMapPass — ACES 色调映射
- **类别**: 后处理
- **原理**: 全屏三角形，HDR `RGBA16_FLOAT` → LDR `BGRA8_UNORM`。支持 AutoExposure 输入。
- **解决问题**: HDR→LDR 转换，模拟胶片响应曲线。

### 53. AutoExposurePass — 自动曝光
- **类别**: 后处理
- **原理**: Compute Shader 亮度直方图降采样（256 线程组）→ SSBO → CPU 端 temporal adaptation（`newLogLum = lerp(prev, avg, adaptSpeed*dt)`）→ 钳位 [0.005, 200.0]。目标亮度 0.18（18% 灰），适应速度 2.0。AsyncCompute 队列运行。
- **解决问题**: 模拟人眼适应明暗环境，HDR 场景自动曝光调节。

### 54. SSAO（屏幕空间环境光遮蔽）
- **类别**: 后处理
- **原理**: Pass 1: 半球采样（64 样本核 + 4×4 噪声纹理贴片）深度/法线测试 → 遮蔽因子。Pass 2: 可分离模糊降噪。输出 R8_UNORM AO 纹理，Lighting Pass binding 20。
- **解决问题**: 无预计算的屏幕空间 AO，增强场景深度感。

### 55. BloomPass — 辉光
- **类别**: 后处理
- **原理**: BrightPass（亮度阈值的明亮像素提取 → 半分辨率） → GaussianBlur（可分 7×7） → Composite（上采样 + 与原 HDR 混合）。懒初始化。
- **解决问题**: 高亮区域光晕效果，增强 HDR 视觉冲击。

### 56. DOFPass — 景深
- **类别**: 后处理
- **原理**: CoC Pass（散光圈直径：焦点深度 + GBuffer 线性深度）→ GaussianBlur → Composite（基于 CoC 混合模糊/清晰）。
- **解决问题**: 电影感景深，屏幕空间实现无需额外几何数据。

### 57. MotionBlurPass — 运动模糊
- **类别**: 后处理
- **原理**: 单 Pass 全屏着色器，读取 GBuffer 速度纹理（RG16_FLOAT，UV 空间运动矢量），沿速度方向采样 12 个样本累积加权颜色。
- **解决问题**: 逐像素运动模糊，利用 GBuffer 速度缓冲区提升低帧率流畅感。

### 58. GaussianBlurPass — 可分高斯模糊
- **类别**: 后处理/构建块
- **原理**: 7×7 可分高斯核（sigma=1.0），硬编码权重。可被 Bloom/DOF/SSAO/Denoiser 复用。
- **解决问题**: 共享的优化模糊组件，避免代码重复。

### 59. Denoiser — 双边滤波去噪
- **类别**: 后处理
- **原理**: 5×5 双边滤波，以深度差 + 法线差为权重（sigma: depth=10.0, normal=8.0），保留边缘的同时平滑平坦区域的噪声。
- **解决问题**: SSGI/SSR 的噪声抑制，不模糊深度/法线边缘。

### 60. ColorGradingPass — 色彩分级
- **类别**: 后处理
- **原理**: LDR 空间全屏着色器，饱和度/对比度/鲜艳度调节。ToneMap 之后、AA 之前。懒初始化。
- **解决问题**: 艺术色彩调节，LDR 空间后处理实现。

### 61. SkyboxPass — 天空盒
- **类别**: 后处理
- **原理**: 全屏三角形，深度测试 `EQUAL`（仅远裁剪面像素着色）。逆 ViewProj 矩阵计算世界空间光线方向 → 采样立方体贴图。
- **解决问题**: 高效天空盒渲染，深度缓冲早期拒绝场景几何遮挡像素。

---

## 十六、抗锯齿

### 62. AA_TAA — 时域抗锯齿
- **类别**: 抗锯齿
- **原理**: HDR 空间，子像素 Halton 序列抖动投影矩阵 → 速度 Buffer 重投影 → GBuffer 深度/法线邻域裁剪（防重影）→ 双缓冲历史纹理 EMA 混合。`OnBeginFrame()` 更新抖动偏移。
- **解决问题**: 高质量的时域累积抗锯齿，利用速度矢量精确重投影。

### 63. AA_FXAA — 快速近似 AA
- **类别**: 抗锯齿
- **原理**: LDR 空间单 Pass，亮度边缘检测 + 局部对比度分析 + 子像素混合平滑。无时域历史。输出直接写 BackBuffer（`OwnsOutput()=false`）。
- **解决问题**: 轻量单采样 AA，无时域伪影，适合作为 TAA 的后备。

### 64. AA_SMAA — 子像素形态学 AA
- **类别**: 抗锯齿
- **原理**: 三 Pass LDR 空间：EdgeDetect（R8_UNORM 边缘纹理）→ BlendWeight（RGBA8_UNORM 四方向混合权重）→ NeighborhoodBlend（写 BackBuffer）。与 FXAA 互斥。
- **解决问题**: 比 FXAA 更高质量的时域无关 AA，精确子像素特征检测。

### 65. AA_MSAA — 硬件多重采样
- **类别**: 抗锯齿
- **原理**: 无独立 RenderPass——参数覆盖系统。重写纹理 `sampleCount` 和 PSO `sampleCount`。默认 4×。延迟渲染仅 HDR 目标使用 MSAA（GBuffer MRT 多采昂贵）。
- **解决问题**: 零额外 Pass 开销（GPU 硬件 resolve），硬件 AA。

---

## 十七、阴影系统

### 66. IShadowSystem / IShadowTechnique
- **类别**: 阴影
- **原理**: 统一抽象支持 3 种模式：None/Traditional/RayTraced。ShadowSystem 组合多个 Technique（CSM + Spot + Point），统一 GPUShadowData 数组管理，按技术边界映射阴影贴图索引。
- **解决问题**: 可组合阴影架构——每种光源类型独立算法，ShadowSystem 统一编排。

### 67. CSM — 级联阴影贴图
- **类别**: 阴影/方向光
- **原理**: 3 级联（CASCADE_COUNT=3），2048² D32_FLOAT 阴影贴图/级联。使用 Gribb-Hartmann 视锥平面提取 + 混合分割方案（lambda=0.5，均匀+对数混合）。每级联独立正交投影渲染。
- **解决问题**: 高质量方向光阴影，近处精细远处粗糙，保持一致纹素密度。

### 68. Point/Spot 阴影
- **类别**: 阴影/点光/聚光
- **原理**: PointShadowTechnique 渲染 6 面 512² D32_FLOAT 立方体贴图。SpotShadowTechnique 渲染单张 1024² 透视阴影贴图（FOV = outerConeAngle*2）。
- **解决问题**: 点光源全向阴影、聚光源透视阴影。

---

## 十八、全局光照（GI）

### 69. IBL — 基于图像的光照
- **类别**: GI
- **原理**: 从天空盒立方体贴图生成三张贴图：辐照度图（32² RGBA16_FLOAT，余弦加权半球卷积）→ 预滤波图（128² RGBA16_FLOAT 立方体贴图 × 5 mip，重要性采样 GGX 卷积）→ BRDF LUT（512² RG16_FLOAT，分裂和积分）。脏标志触发重建。
- **解决问题**: 物理正确环境光照（漫反射+镜面），分裂和近似实现实时 PBR。

### 70. SSGI / SSR — 屏幕空间 GI/反射
- **类别**: GI/屏幕空间
- **原理**: SSGI：随机方向半球光线行进，采样 GBuffer 深度检测交点，贡献反照率为间接漫反射。SSR：镜面反射方向光线行进，采样 GBuffer 深度。均通过 Denoiser 去噪后供 Lighting Pass 采样。
- **解决问题**: 实时屏幕空间间接光照/反射，无预计算，支持动态场景。

### 71. DDGI — 动态漫反射 GI
- **类别**: GI/探针
- **原理**: 3D 探针网格（8×4×8，cellSize=3.0），球谐(SH)存储辐照度（16 float4/探针：9 SH 系数 + 7 保留）。Compute Shader 逐帧更新：GBuffer 屏幕采样投影到 SH + 与上一帧历史 temporal blend（85% 保留率）。Lighting Pass 三线性插值 8 个最近探针。AsyncCompute 运行。
- **解决问题**: 动态实时漫反射 GI，无需光照烘焙，支持移动光源和动态几何。

### 72. RSM — 反射阴影贴图 GI
- **类别**: GI
- **原理**: 从方向光源视角渲染位置(RGBA16_FLOAT) + 通量(RGBA16_FLOAT)到 512² 纹理。Lighting Pass 采样以近似直接光单次反弹漫反射。
- **解决问题**: 实时单次反弹漫反射 GI，利用现有阴影贴图渲染。

---

## 十九、GPU 驱动渲染

### 73. GPUScene
- **类别**: GPU 驱动
- **原理**: 统一 SSBO，每物体 128 字节（std430）：localToWorld 矩阵 + 包围盒 + 网格/材质索引 + 间接绘制参数。`Collect()` 遍历 World；`Upload()` 脏标志增量上传（仅变更物体推送到 GPU）。容量 `kMaxGPUObjects=2048`。
- **解决问题**: 单 SSBO 表示整个可见场景，GPU Compute Shader 遍历所有物体进行剔除/LOD/间接绘制。

### 74. MeshBatcher
- **类别**: GPU 驱动
- **原理**: 合并所有 StaticVertex 网格到共享 VB/IB → 记录 IndirectDrawCommand/DGCDrawToken 数组。DGC 模式输出 `DGCDrawToken`（36 字节，含 objectIndex）。
- **解决问题**: 单次 DrawIndexedIndirect 批量渲染，GPU 剔除着色器直接写绘制命令。

### 75. GPUCulling — GPU 剔除
- **类别**: GPU 驱动
- **原理**: Compute Shader 执行视锥（Gribb-Hartmann 6 平面） + Hi-Z 遮挡剔除。三模式：单阶段（每帧 Dispatch）、两阶段（Phase 1 粗筛 → Hi-Z 构建 → Phase 2 精筛，AsyncCompute）、PTG（持久线程组，单工作组持续运行等帧信号）。
- **解决问题**: 视锥+遮挡剔除完全 GPU 化，GPU 自主生成绘制命令。

### 76. Hi-Z 金字塔
- **类别**: GPU 驱动
- **原理**: R32_FLOAT 纹理 × 最多 8 mip 级别。每个 mip 是上一级的保守最大深度（2×2 取 max）。HiZDownsample.comp 计算着色器逐级构建。每个 mip 配备存储视图（计算写入）+ 采样视图（着色器读取），点采样器保证精确纹素访问。
- **解决问题**: 多分辨率深度表示，高效层级遮挡查询——单次采样可拒绝大区域。

### 77. PTG — 持久线程组
- **类别**: GPU 驱动
- **原理**: 单个 64 线程计算工作组持续循环（PersistentCull.comp），spin-wait 帧索引计数器。CPU 写入每帧参数到 Uniform Buffer → 着色器处理 → 递增计数器返回。关闭信号 `frameIndex=0xFFFFFFFF`。
- **解决问题**: 消除每帧 Dispatch 开销，计算着色器常驻 GPU。

### 78. ClusteredShading — 分簇着色
- **类别**: 光照优化
- **原理**: 视锥体划分为 3D 网格（64px 屏幕 tiles × 12 对数深度切片）。CPU 端 AABB 与光源球体相交测试 → LightGrid（偏移+计数）+ LightIndexList 上传 GPU SSBO。Lighting Pass 着色器像素→簇索引→遍历簇内光源。
- **解决问题**: 逐像素光照复杂度 O(N) → O(簇内光源数)，大量动态光源场景必备。

---

## 二十、光线追踪

### 79. RTPass — 光线追踪管线
- **类别**: 光线追踪
- **原理**: 管理完整 RT 管线：BLAS（每 MeshComponent，几何哈希增量重建）→ TLAS（每帧所有实例 × 4×3 变换矩阵）→ 着色器绑定表（SBT: RayGen/Miss/Hit/Callable 槽）→ TraceRays 调度。与 bindless 纹理集成（set=2）。
- **解决问题**: 硬件光追管线封装，支持 RT 阴影等。

### 80. SBT — 着色器绑定表
- **类别**: 光线追踪
- **原理**: 四个 strided 设备地址区域（RayGen/Miss/Hit/Callable），由 VulkanCommandList::TraceRays() 构建。由相邻偏移量计算区域大小。
- **解决问题**: 正确构建交错 SBT 地址区域，在 Vulkan 中分派光线。

---

## 二十一、粒子系统

### 81. GPU 粒子管线
- **类别**: 粒子
- **原理**: 完全 GPU 驻留：Init（初始化缓冲区）→ Emit（发射参数生成）→ Simulate（位置/速度/生命周期更新 + 渐变纹理）→ Sort（深度排序，正确混合）→ Culling（视锥剔除）→ Render（公告板四边形 + 软粒子深度测试）。所有阶段使用间接调度/绘制。
- **解决问题**: GPU 驱动粒子系统，消除 CPU readback 粒子计数。支持软粒子、渐变纹理。

### 82. 软粒子
- **类别**: 粒子
- **原理**: 片段着色器采样场景深度 → 计算粒子与几何的深度差 → 阈值内平滑衰减 alpha。点采样器避免插值伪影。
- **解决问题**: 消除粒子与场景几何的硬边缘交叉——烟雾/火焰/雾气的关键质量特性。

---

## 二十二、GPU WorkGraph

### 83. GPUWorkGraph — 软件工作图
- **类别**: 工作图
- **原理**: 使用计算着色器+原子计数器模拟 GPU WorkGraph。三种节点：Entry（读入输出记录）、Compute（通用计算）、Draw（终端，读记录输出统计数据）。WGRecord=32 字节 payload（nodeID+7×u32）。节点间通过 SSBO 记录队列通信。按注册顺序遍历执行。
- **解决问题**: 无需硬件支持的工作图概念原型（VK_AMDX_shader_enqueue），为未来硬件工作图探路。

---

## 二十三、BindlessTextureManager

### 84. BindlessTextureManager — 全局无绑定纹理数组
- **类别**: 资源绑定
- **原理**: 全局单例管理 Texture2D[4096] + SamplerState[4096]（set=0 binding 5-6）。每材质占 4 个连续索引（BaseColor/Normal/MetallicRoughness/Occlusion）。`RegisterDescriptorSet()` + `RegisterMaterial()` + `FlushPending()`（脏标志批量推送到所有注册描述符集）。
- **解决问题**: 消除逐材质描述符集切换——所有材质通过无绑定索引访问，GPU 驱动渲染必需。

---

## 二十四、ShaderHotReload（着色器热重载）

### 85. ShaderHotReload
- **类别**: 开发工具
- **原理**: 后台线程 `ReadDirectoryChangesW` 监控 `.slang` 文件变化（200ms 去抖）→ `slangc` 重编译 SPIR-V → 互斥队列传递结果 → 主线程 `Poll()` 消费 → `ReloadCallback` 触发 PSO 热替换（创建新 PSO + 旧 PSO 延迟 3 帧销毁）。
- **解决问题**: 即时迭代着色器，300ms 内看到效果，无需重启应用。

---

## 二十五、Shader 编译管线

### 86. Slang → SPIR-V → C++ 头文件嵌入
- **类别**: 构建系统
- **原理**: CMake 脚本显式列出所有 `.slang` 文件 → `slangc -target spirv` 编译 → `spv_to_header.py` 将 `.spv` 转为 `uint32_t[]` C++ 头文件。RT 着色器按扩展名（`.rgen`/`.rmiss`/`.rchit`）自动映射 stage。`CompileShaders` 目标确保在所有构建步骤前完成。
- **解决问题**: 着色器字节码嵌入可执行文件，无运行时文件加载。

### 87. ShaderTypes.slang — C++/GPU 共享类型
- **类别**: 着色器编译
- **原理**: 单文件，条件宏分隔 C++（uint32_t/alignas）和 Slang（uint/默认对齐）。定义 GPU 常量（描述符集/绑定索引、kGPUMaxLights=8、kGPUMaxGPUObjects=2048）和结构体（GPULight/GPUShadowData/GPUObjectData/PushConstantData）。
- **解决问题**: C++ 和着色器共享同一结构体定义，消除手动对齐错误。

---

## 二十六、Profiler（性能分析器）

### 88. ProfilerManager — GPU 时间戳分析
- **类别**: 性能分析
- **原理**: 三缓冲时间戳查询池（每 Pass 2 时间戳 × 3 帧槽位）。Frame N 写入 → Frame N+2 读回（3 帧延迟保证 GPU 完成）。GPU tick → 毫秒转换（`timestampPeriod`）。可选管线统计（6 计数器：输入顶点/图元、VS/FS 调用、裁剪调用/图元），派生指标：VS:FS 比率、图元裁剪率、每顶点平均片段数。
- **解决问题**: 精确逐 Pass GPU 计时，无 CPU 开销。三缓冲避免立刻读回阻塞。

### 89. ProfilerPanel — ImGui 可视化
- **类别**: 性能分析/UI
- **原理**: 三种视图：(1) Timeline（水平条形图，绿→红渐变）(2) Heatmap（颜色编码表）(3) History（最近 120 帧折线图，CSV 导出）。支持 Pass 预算检测（`SetPassBudget` + `CheckBudgets`）。
- **解决问题**: 引擎内实时 GPU 分析，无需外部工具，支持历史趋势和预算违规告警。

---

## 二十七、场景管理

### 90. ECS World — 实体-组件容器
- **类别**: 场景管理
- **原理**: Entity 分配递增 UUID。组件通过模板方法管理（`AddComponent<T>` → `OnCreate`/`OnStart` 生命周期；`GetComponent<T>`；`ForEach<T>(callback)`迭代）。基于 `std::type_index` 桶存储。
- **解决问题**: 数据驱动场景管理，组件可组合，支持反射序列化。

### 91. SceneGraph — 分层变换
- **类别**: 场景管理
- **原理**: 层次变换系统：节点存储父实体 + 子实体列表 + 局部/世界矩阵 + 脏标志。`MarkDirty(entity)` 传播到整个子树。`UpdateTransforms()` 懒更新（仅脏节点重计算）。T*R*S 矩阵合成。
- **解决问题**: 带脏标志传播的层级变换，避免每帧完全遍历。

### 92. glTF 2.0 Loader — 场景加载
- **类别**: 资产
- **原理**: 通过 cgltf 解析 `.glb`/`.gltf` → 处理节点层次、TRS 变换、PBR 材质参数、纹理路径提取、字节跨度/稀疏访问器。返回 `glTFResult`（实体列表、网格计数、错误信息）。
- **解决问题**: 标准 glTF 2.0 资产加载，支持 Sponza 等复杂场景。

### 93. AnimationComponent — 关键帧动画
- **类别**: 场景管理
- **原理**: `AnimKey<T>` 模板支持 Translation(float3)/Rotation(quat)/Scale(float3) 独立轨道。`TransformClip` 封装命名片段（持续时间、循环、关键帧列表）。`Update(deltaTime, Transform)` 关键帧插值驱动。
- **解决问题**: 基于关键帧的 Transform 动画系统。

---

## 二十八、编辑器系统

### 94. CVar — 控制台变量
- **类别**: 编辑器
- **原理**: `CVar<T>`（i32/f32/String/bool）构造函数自动注册到全局列表。`Get()`/`Set()`/`SetFromString()` + `FindCVar(name)` 查找。
- **解决问题**: 运行时控制台变量，热调节渲染参数。

### 95. CommandHistory — 撤销/重做
- **类别**: 编辑器
- **原理**: 命令模式：`Command` 基类定义 `Execute()`/`Undo()`。`CommandHistory` 维护双栈（各最多 256 条）。`PropertyChangeCommand` 用 lambda 捕获通用撤销/重做。
- **解决问题**: 编辑器撤销/重做系统。

### 96. ImGuiIntegration — 编辑器 UI
- **类别**: 编辑器
- **原理**: 管理 ImGui 生命周期 + GLFW 输入 + Vulkan 渲染。Vulkan 特定资源（描述符池/渲染通道）通过 RHI 后端辅助接口创建，保持后端中立。
- **解决问题**: 即时模式 GUI 的后端无关集成。

### 97. SceneSerializer — 场景序列化
- **类别**: 编辑器
- **原理**: 二进制 `.hescene` 格式（HESC 魔数 + 版本号），通过反射系统遍历 `PF_Serializable` 属性序列化/反序列化。
- **解决问题**: 编辑器场景保存/加载。

---

## 二十九、硬件特性

### 98. Mesh Shading
- **类别**: 硬件特性/VK_EXT_mesh_shader
- **原理**: 动态加载 `vkCmdDrawMeshTasksEXT`/`vkCmdDrawMeshTasksIndirectEXT` + 查询 `VkPhysicalDeviceMeshShaderPropertiesEXT`（maxWorkGroupInvocations/outputVertices/outputPrimitives/taskInvocations/payloadSize）。绕过传统 IA 阶段。
- **解决问题**: GPU 驱动几何处理，支持 Task+Mesh 着色器管线。

### 99. Ray Tracing
- **类别**: 硬件特性/VK_KHR_acceleration_structure + VK_KHR_ray_tracing_pipeline
- **原理**: 完整的硬件 RT 管线：BLAS（每 mesh 底级 AS）+ TLAS（每帧实例顶级 AS）+ SBT（RayGen/Miss/Hit/Callable）+ vkCmdTraceRays。
- **解决问题**: 硬件加速光线追踪，用于 RT 阴影、反射等。

### 100. DGC（VK_EXT_device_generated_commands）
- **类别**: 硬件特性
- **原理**: 7 个扩展函数指针动态加载。管理 IndirectCommandsLayout + IndirectExecutionSet + 预处理缓冲区。支持 vkCmdExecuteGeneratedCommandsEXT。
- **解决问题**: GPU 自主生成绘制命令，消除 CPU-GPU 往返。

### 101. AsyncCompute
- **类别**: 硬件特性
- **原理**: Tier 1 独立 Compute 队列族。RenderGraph 自动将 Compute Pass 分配到 Compute 队列 → Timeline Semaphore 跨队列同步。
- **解决问题**: GPU 硬件并行：图形和计算在独立队列上并发执行。

### 102. Compute Shader Derivatives（VK_KHR_compute_shader_derivatives）
- **类别**: 硬件特性
- **原理**: 设备创建时检测并启用，允许 Compute Shader 使用 `ddx`/`ddy` 导数运算。
- **解决问题**: 粒子 SSAO 等 CS 中需要屏幕空间导数的技术。

---

## 三十、第三方库

| 库 | 用途 |
|---|---|
| **GLFW** | 窗口创建、输入 |
| **GLM** | 数学库（向量、矩阵、四元数） |
| **spdlog** | 日志系统 |
| **Taskflow** | 作业/任务调度 |
| **Vulkan SDK** | GPU API (v1.4.341.1) |
| **VMA** | GPU 内存分配器（单头库） |
| **Slang (slangc)** | 着色器编译器（SPIR-V 输出） |
| **ImGui** | 编辑器 UI 框架 |
| **meshoptimizer** | 网格优化（顶点缓存/过度绘制） |
| **stb** | 图像加载（stb_image） |
| **cgltf** | glTF 2.0 解析 |

---

> 共 102 个技术名词，覆盖 HugEngine 全部核心系统。
> 生成日期：2026-07-22
