# HugEngine RHI 架构与 Vulkan 实现分析

> 分析日期：2026-09-21 | 分支：main | 分析范围：`Engine/RHI/` 全部源文件

---

## 一、总体架构

### 1.1 设计哲学

RHI (Rendering Hardware Interface) 是 HugEngine 的 **L2 渲染硬件抽象层**。其核心原则是：

> **只有 RHI 模块可以直接调用 Vulkan / D3D12 等图形 API。引擎其他所有模块（Core、Scene、Render、Editor 等）以及 Sample 必须通过 `IRHI*` 公共接口间接使用 GPU 功能，不得直接依赖任何图形 API 头文件。**

这是一个经典的 **后端抽象模式**（类似 UE 的 RHI、Filament 的 Driver API），通过纯虚接口 + 工厂方法实现多后端支持。

### 1.2 分层结构

```
┌───────────────────────────────────────────────┐
│  Engine 上层 (Scene/Render/Editor/Sample)       │
│  只依赖 RHI/RHI.h 公共头文件                      │
├───────────────────────────────────────────────┤
│           RHI 公共接口层 (RHI/*.h)                │
│  IRHIDevice, IRHIBuffer, IRHITexture,           │
│  IRHICommandList, IRHIPipelineState, ...         │
├───────────────────────────────────────────────┤
│         Vulkan 后端 (Vulkan/*.cpp)               │
│  VulkanDevice, VulkanBuffer, VulkanTexture,     │
│  VulkanCommandList, VulkanPipelineState, ...     │
├───────────────────────────────────────────────┤
│  第三方: Vulkan SDK + VMA (VulkanMemoryAllocator)│
└───────────────────────────────────────────────┘
```

### 1.3 文件组织

| 层次 | 目录 | 职责 |
|------|------|------|
| **公共接口** | `Engine/RHI/RHI/` | 纯虚接口类 (IRHI*) + 枚举/结构体 |
| **Vulkan 后端** | `Engine/RHI/Vulkan/` | 所有 `Vulkan*` 实现类 |
| **构建系统** | `Engine/RHI/CMakeLists.txt` | 静态库 `HugEngineRHI`，条件编译 Vulkan 后端 |

### 1.4 Vulkan 后端文件职责划分

| 文件 | 行数 | 职责 |
|------|------|------|
| `VulkanDevice.cpp` | ~1360 | 设备生命周期、VkInstance/Device 创建、队列管理、资源创建委托、PSO 缓存磁盘持久化、VulkanDeviceAccess 桥接 |
| `VulkanDevice_Descriptors.cpp` | ~525 | DescriptorSet 布局/分配/更新（5 种 UpdateDescriptorSet 重载）、Per-Mip ImageView |
| `VulkanDevice_MeshShader.cpp` | ~170 | Mesh Shader + DGC 能力查询、扩展函数加载 |
| `VulkanDevice_GPL.cpp` | ~65 | Graphics Pipeline Library (VK_EXT_graphics_pipeline_library) 能力查询 |
| `VulkanDevice_DebugUtils.cpp` | ~100 | Debug Utils 函数加载、资源调试命名 |
| `VulkanSwapChain.cpp` | ~285 | 交换链：创建/销毁、窗口缩放、图像获取、呈现、深度缓冲自管理、HDR10 格式选择 |
| `VulkanCommandList.cpp` | ~1140 | 命令录制：构造/析构、Begin/End、状态设置、Draw/Dispatch（含 IndirectCount 变体）、Barrier、Copy、Mesh Shader 命令、RT 命令、Debug Label |
| `VulkanCommandList_RenderPass.cpp` | ~560 | 渲染通道：SwapChain RP (Clear/Load)、离屏单附件、离屏 MRT (最多 8 颜色 + 1 深度)、延迟 FB 销毁 |
| `VulkanCommandList_Submit.cpp` | ~225 | Submit：跨队列所有权转移、Timeline Semaphore 集成、队列提交 |
| `VulkanResources.cpp` | ~785 | 资源实现：Buffer (持久映射 + VMA)、Texture (含 mipmap 生成 + Cubemap 支持)、Sampler、格式转换 |
| `VulkanPipeline.cpp` | ~820 | PSO 创建：Graphics/Compute/Mesh 管线构建、RenderPass、PipelineLayout、PSO 缓存查找与 GPL fast-link |
| `VulkanPipelineLibrary.cpp` | ~245 | GPL 四段库缓存（VertexInput/PreRaster/FragmentShader/FragmentOutput）与 fast-link |
| `VulkanRT.cpp` | ~525 | Ray Tracing：VulkanAccelerationStructure (BLAS/TLAS)、VulkanRTPipelineState、扩展函数加载、能力查询 |
| `VulkanDGC.cpp` | ~230 | Device Generated Commands：Layout/ExecutionSet/PreprocessBuffer 创建 |
| `VulkanBindlessHeap.cpp` | ~100 | 设备级 bindless 描述符堆（纹理/采样器/SSBO 槽位环形分配 + 批量 Flush） |
| `VulkanTextureLiveness.cpp` | ~45 | 纹理存活登记（描述符更新前识别已释放纹理句柄） |
| `PSOPrecompileManager.cpp` | ~450 | PSO 后台预热管理器（独立 VkPipelineCache + 合并回主缓存） |
| `DeferredDestructionQueue.h` | ~95 | 延迟销毁队列（Framebuffer / Pipeline 按帧延迟释放） |
| `TransientResourceAllocator.cpp` | ~285 | 瞬态资源分配器（128MB × 2 Heap、帧内 bump 子分配 + VkImage 缓存） |

---

## 二、公共接口层详细分析

### 2.1 核心接口体系

```
IRHIDevice                 ← 设备工厂（创建所有资源 + 提交命令）
├── IRHISwapChain          ← 交换链（窗口表面、呈现）
├── IRHICommandList        ← 命令缓冲（录制 + 提交所有 GPU 命令）
├── IRHIBuffer             ← GPU 缓冲（顶点/索引/Uniform/Storage/间接）
├── IRHITexture            ← GPU 纹理（含 Cubemap 逐面 ImageView）
├── IRHISampler            ← GPU 采样器（含深度比较采样器）
├── IRHIPipelineState      ← 管线状态对象（Graphics/Compute/Mesh PSO）
├── IRHIRayTracingPipelineState ← 光追管线状态
├── IRHIAccelerationStructure   ← BLAS/TLAS 加速结构
└── IRHIQueryPool          ← GPU 查询池（时间戳 / 管线统计）
```

### 2.2 枚举与结构体设计 (`Types.h`)

**设备后端** — `Backend`: Vulkan, D3D12, Metal, WebGPU（目前仅实现 Vulkan）

**队列类型** — `QueueType`: Graphics, Compute, Copy（三级队列抽象）

**资源状态** — `ResourceState` 采用 **位掩码设计**（可组合）：
- 读写分离：`DepthStencilRead` / `DepthStencilWrite` 独立
- 覆盖常见管线阶段：`ShaderResource`, `RenderTarget`, `UnorderedAccess`, `CopySrc/Dst`, `Present`
- 声明为 `enum class` 配合位运算重载

**管线阶段** — `PipelineStage` 同样是位掩码，支持组合屏障
- 涵盖：TopOfPipe, DrawIndirect, VertexInput, VertexShader, FragmentShader, Early/LateFragmentTests, ColorAttachmentOutput, ComputeShader, Transfer, BottomOfPipe, RayTracingShader, AccelerationStructureBuild

**访问标志** — `AccessFlags` 位掩码，细粒度读写控制

**描述符系统** — 使用 `u64` 不透明句柄：
```cpp
using DescriptorSetLayoutHandle = u64;  // Vulkan 内部索引
using DescriptorSetHandle        = u64;
using RHIFenceHandle            = u64;  // Timeline Semaphore
constexpr kInvalidLayout = 0;           // 空句柄哨兵值
```

### 2.3 `IRHIDevice` — 设备接口

这是最大的接口（~180 行），负责：

| 职责 | 方法数 | 关键方法 |
|------|--------|----------|
| 资源创建 | 14 | CreateBuffer / CreateTexture / CreateTransientTexture / CreateSampler / CreatePSO / CreateSwapChain / CreateCommandList / CreateSecondaryCommandList / CreateBLAS / CreateTLAS / GetBLASBuildSizes / GetTLASBuildSizes / CreateRTPSO / CreateQueryPool |
| 描述符集管理 | 8 | CreateDescriptorSetLayout / AllocateDescriptorSet / UpdateDescriptorSet ×5 / UpdateDescriptorSetWithImageView / DestroyDescriptorSetLayout |
| 多队列支持与跨队列同步 | 8 | HasAsyncComputeQueue / GetQueueFamily / CreateFence / DestroyFence / WaitForFence / GetFenceValue / SignalFenceOnQueue / WaitFenceOnQueue |
| 命令提交 | 3 | Submit (单个) / SubmitAll (批量，按队列分组) / WaitIdle |
| Per-Mip / 逐层视图 | 5 | CreateTextureMipStorageView ×2 (mip / mip+layer) / CreateTextureMipSampledView ×2 / DestroyTextureMipView |
| Bindless 与瞬态资源 | 3 | GetBindlessHeap / CreateTransientTexture / AdvanceTransientResources |
| PSO 预热与创建限流 | 6 | PrecompileQueuePSO / StartPSOPrecompile / GetPSOPrecompileProgress / EnqueuePSOCreate / ProcessPSOCreateQueue / GetPendingPSOCreateCount |
| 调试命名 | 5 | SetResourceDebugName (Buffer / Texture / PSO / Sampler / AS) |
| DGC 与后端辅助 | 11 | InitializeDGC / ShutdownDGC / IsDGCReady / GetDGC* ×4 / CreateImGui* ×4 |
| 查询与格式能力 | 3 | CreateQueryPool / GetTimestampPeriod / SupportsStorageImage |

**全局设备访问** — 通过单例模式：
```cpp
IRHIDevice* GetDevice();                         // 全局获取
void SetDevice(IRHIDevice* device);              // 初始化时设置
std::unique_ptr<IRHIDevice> CreateDevice(Backend); // 工厂，Vulkan: new VulkanDevice()
```

### 2.4 `IRHICommandList` — 命令缓冲接口

最复杂的接口（~250 行），涵盖十一类：

| 类别 | 方法 |
|------|------|
| **生命周期** | Begin / BeginLightweight / End / BeginSecondary / ExecuteSecondary / IsSecondary |
| **渲染通道** | BeginRenderPass / BeginOffscreenPass / BeginOffscreenPassMRT / EndRenderPass / EndOffscreenPass |
| **状态设置** | SetSwapChain / SetPipeline / SetVertexBuffer / SetIndexBuffer / SetViewport / SetScissor / BindDescriptorSet / SetPushConstants |
| **绘制** | Draw / DrawIndexed / DrawIndexedIndirect / DrawIndexedIndirectCount / DrawMeshTasks / DrawMeshTasksIndirect / DrawMeshTasksIndirectCount / ExecuteGeneratedCommands |
| **计算** | Dispatch / DispatchIndirect |
| **光追** | BuildBLAS / BuildTLAS / BindRTPipeline / TraceRays |
| **屏障** | PipelineBarrier ×2 (内存/图像) / QueueOwnershipTransfer ×2 (纹理/缓冲) / ReleaseToQueue / AcquireFromQueue |
| **拷贝与清除** | CopyBuffer / CopyTextureToTexture / CopyTextureToBuffer / ClearDepthStencil |
| **查询** | WriteTimestamp / ResetQueryPool / GetQueryResults / TryGetQueryResults / BeginQuery / EndQuery |
| **调试标签** | BeginDebugLabel / EndDebugLabel / InsertDebugLabel / SetDrawDebugLabel / ClearDrawDebugLabel |
| **同步** | SetTimelineSignal / SetTimelineWait / Submit |

### 2.5 `PipelineStateDesc` — 管线状态描述

支持三种管线类型的分派逻辑：
- `bindPoint == Compute` → 计算管线
- `meshShader != nullptr && !meshShader->spirv.empty()` → Mesh Shader 管线
- 其他 → 传统 Graphics 管线 (Vertex + Fragment)

关键字段：
- **MRT 支持**：`colorFormats[8]`，最多 8 个颜色附件
- **Subpass 索引**：`subpassIndex`（用于 Deferred 渲染多 Subpass）
- **LoadOp 控制**：`colorLoadOp` / `depthLoadOp`（Clear / Load）
- **Push Constant 范围**：`std::vector<PushConstantRange>`，stageMask 使用跨平台 `kStageMask*` 位掩码（当前取值与 Vulkan VkShaderStageFlagBits 一致）
- **光栅化/深度/混合状态**：`cullMode` / `frontFace` / `fillMode` / `depthTest` / `depthWrite` / `depthCompare` / `colorBlend[8]`（每 MRT 独立混合状态）
- **MSAA**：`sampleCount` 字段（Vulkan 后端 PSO 仍固定 `SAMPLE_COUNT_1_BIT`）
- **DGC**：`indirectBindable`（以本管线作为 DGC 执行集 initialPipeline 时必需）
- **描述符集布局**：`std::vector<DescriptorSetLayoutHandle>`（预创建句柄数组）

### 2.6 其他公共接口

| 接口 | 文件 | 关键成员 |
|------|------|----------|
| `IRHIBuffer` | `Buffer.h` | GetSize / Map / Unmap / GetDeviceAddress |
| `IRHITexture` | `Buffer.h` | GetWidth/Height/Depth/MipLevels/ArrayLayers/Format / GetNativeHandle(index) |
| `IRHISampler` | `Buffer.h` | 空接口（仅析构） |
| `IRHISwapChain` | `SwapChain.h` | Resize / AcquireNextImage / Present / GetCurrentBackBufferView / GetDepthBufferView |
| `IRHIPipelineState` | `Shader.h` | GetNativeHandle (void* → VkPipeline) |
| `IRHIQueryPool` | `QueryPool.h` | GetQueryCount |
| `IRHIAccelerationStructure` | `RayTracing.h` | GetDeviceAddress / GetSize |
| `IRHIRayTracingPipelineState` | `RayTracing.h` | GetShaderGroupCount / GetShaderGroupHandleSize / GetShaderGroupHandles |

---

## 三、Vulkan 后端实现分析

### 3.1 `VulkanDevice` — 设备生命周期管理

#### 初始化流程（11 步）

```
Initialize(DeviceInitDesc)
  ├─ 1. vkCreateInstance
  │    ├─ Vulkan 1.3 API 版本
  │    ├─ 扩展: VK_KHR_surface + VK_KHR_win32_surface
  │    ├─ 扩展: VK_EXT_debug_utils（**始终加载**，对象命名/调试标签不依赖验证层）
  │    └─ 可选: VK_LAYER_KHRONOS_validation（仅在 enableValidation 时）
  │
  ├─ 2. 设置 DebugUtils Messenger（仅 Validation 模式）
  │    └─ 回调级别: WARNING | ERROR
  │    └─ 消息类型: GENERAL | VALIDATION | PERFORMANCE
  │
  ├─ 3. CreateSurface (Win32 HWND)
  │    └─ vkCreateWin32SurfaceKHR(hinstance, hwnd)
  │
  ├─ 4. SelectPhysicalDevice
  │    └─ 优先 VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
  │
  ├─ 5. 硬件能力检测（在创建逻辑设备之前）
  │    ├─ QueryRTCapabilities()
  │    │   └─ 检查 扩展: AS + RTPipeline + DeferredHostOps + PositionFetch
  │    │   └─ 查询属性: maxRayRecursionDepth, shaderGroupHandleSize, AS 属性
  │    ├─ QueryMeshCapabilities()
  │    │   └─ 检查扩展: VK_EXT_mesh_shader
  │    │   └─ 查询属性: maxMeshWorkGroupInvocations, maxMeshOutputVertices/Primitives 等
  │    ├─ QueryDGCCapabilities()
  │    │   └─ 检查扩展: VK_EXT_device_generated_commands
  │    │   └─ 查询属性: maxIndirectPipelineCount, maxIndirectSequenceCount 等
  │    ├─ QueryGPLCapabilities()
  │    │   └─ 检查扩展: VK_EXT_graphics_pipeline_library
  │    └─ FindQueueFamilies()
  │        └─ Graphics 族: GRAPHICS | COMPUTE + Present 支持（必须）
  │        └─ AsyncCompute 族检测（三级）:
  │           Tier 2: 纯 Compute（无 GRAPHICS, 无 TRANSFER）
  │           Tier 1: Compute + Transfer（无 GRAPHICS）
  │           Tier 0: 与 Graphics 共享队列
  │
  ├─ 6. CreateLogicalDevice
  │    ├─ 队列信息: Graphics 族 + (条件) Compute 族
  │    ├─ 必需扩展:
  │    │   └─ VK_KHR_swapchain
  │    │   └─ VK_EXT_descriptor_indexing (Bindless 基础)
  │    ├─ 条件扩展 (基于硬件检测):
  │    │   ├─ VK_KHR_acceleration_structure + VK_KHR_ray_tracing_pipeline
  │    │   │   └─ (+ VK_KHR_deferred_host_operations, VK_KHR_ray_tracing_position_fetch)
  │    │   ├─ VK_EXT_mesh_shader
  │    │   ├─ VK_EXT_device_generated_commands (+ VK_KHR_maintenance5，其依赖扩展)
  │    │   ├─ VK_KHR_maintenance7（render pass 内混录 inline + secondary）
  │    │   ├─ VK_EXT_vertex_attribute_robustness（缺失顶点属性读默认值）
  │    │   ├─ VK_KHR_pipeline_library + VK_EXT_graphics_pipeline_library
  │    │   └─ VK_KHR_compute_shader_derivatives
  │    ├─ 核心特性: multiDrawIndirect / independentBlend / fragmentStoresAndAtomics / geometryShader
  │    ├─ pNext 特性链 (自底向上):
  │    │   └─ Vulkan11(shaderDrawParameters) → Vulkan12(descriptorIndexing + bufferDeviceAddress
  │    │      + timelineSemaphore + shaderInt8 + drawIndirectCount) → Vulkan13(dynamicRendering)
  │    │      → [RT] → [Mesh] → [DGC] → [Maintenance7] → [VertexAttributeRobustness] → [GPL] → [ComputeDerivatives]
  │    ├─ 加载扩展函数指针:
  │    │   ├─ LoadRTFunctions(): 8 个 RT 函数
  │    │   ├─ LoadMeshFunctions(): 3 个 Mesh 函数
  │    │   ├─ LoadDGCFunctions(): 7 个 DGC 函数
  │    │   └─ LoadDebugUtilsFunctions(): 4 个调试工具函数
  │    ├─ 获取队列句柄: vkGetDeviceQueue (Graphics + 条件 Compute)
  │    └─ 创建 VMA 分配器:
  │        ├─ flags: VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT
  │        ├─ vulkanApiVersion: VK_API_VERSION_1_3
  │        └─ 手动绑定 vmaVulkanFunctions (避免 vkGetInstanceProcAddr 动态查找)
  │
  ├─ 7. CreateCommandPools
  │    ├─ Graphics 命令池 (family=m_GraphicsFamily, RESET_COMMAND_BUFFER_BIT)
  │    └─ Compute 命令池  (family=m_ComputeFamily)
  │
  ├─ 8. LoadPipelineCache
  │    └─ 从磁盘读取 VkPipelineCache（pipeline_cache.bin），驱动端编译缓存持久化
  │
  ├─ 9. 初始化 TransientResourceAllocator
  │    └─ 128MB × 2 双缓冲 Heap（瞬态纹理内存池）
  │
  ├─ 10. 初始化 PSOPrecompileManager
  │    └─ 绑定主 VkPipelineCache，供后台预热线程编译 PSO 变体
  │
  └─ 11. 初始化 PipelineLibraryCache
       └─ GPL 四段库缓存（依赖 VK_EXT_graphics_pipeline_library）
```

#### pNext 特性链构建方式

使用指针操作手动构建 pNext 链，而非 VkPhysicalDeviceFeatures2 一步查询：

```cpp
void** ppNext = &vulkan11Features.pNext;
*ppNext = &vulkan12Features;    ppNext = &vulkan12Features.pNext;
*ppNext = &vulkan13Features;    ppNext = &vulkan13Features.pNext;
if (m_SupportsRT) {
    *ppNext = &asFeature;           ppNext = &asFeature.pNext;
    *ppNext = &rtPipelineFeature;   ppNext = &rtPipelineFeature.pNext;
    if (m_SupportsRTPositionFetch) {
        *ppNext = &posFetchFeature; ppNext = &posFetchFeature.pNext;
    }
}
if (m_SupportsMesh) { *ppNext = &meshFeature;   ppNext = &meshFeature.pNext; }
if (m_SupportsDGC)  { *ppNext = &dgcFeature;    ppNext = &dgcFeature.pNext; }
if (m_SupportsMaintenance7) { *ppNext = &maint7Feature; ppNext = &maint7Feature.pNext; }
if (m_SupportsVertexAttributeRobustness) {
    *ppNext = &vertexAttrRobustnessFeature; ppNext = &vertexAttrRobustnessFeature.pNext;
}
if (m_SupportsGPL)  { *ppNext = &gplFeature;    ppNext = &gplFeature.pNext; }
if (hasDerivatives) { *ppNext = &computeDerivativesFeature; ppNext = &computeDerivativesFeature.pNext; }
*ppNext = nullptr;
```

注意：DescriptorIndexing / BufferDeviceAddress / TimelineSemaphore / ShaderInt8 等 Vulkan 1.2 特性
已**合并**进同一个 `VkPhysicalDeviceVulkan12Features`（校验规则 VUID-VkDeviceCreateInfo-pNext-02830
禁止 pNext 链里同时出现该结构与其别名结构体），逐字段沿用同一份查询结果。

优点：清晰的条件链构建，无冗余特性声明。

#### 设备销毁（逆序）

```
Shutdown()
  ├─ vkDeviceWaitIdle              (等待 GPU 完成所有工作)
  ├─ SavePipelineCache             (把 VkPipelineCache 写回磁盘)
  ├─ m_TransientAllocator.Shutdown (回收所有瞬态 Heap)
  ├─ m_PSOPrecompileManager.Shutdown (等待后台预热线程 + 销毁 worker cache)
  ├─ m_PipelineLibraryCache.Shutdown (销毁 GPL 四段库)
  ├─ m_PSOCache.clear()            (销毁所有缓存的 PSO)
  ├─ m_DGC.Shutdown                (销毁 DGC Layout / ExecutionSet / PreprocessBuffer)
  ├─ m_DeferredDestroy.FlushAll()  (执行延迟销毁队列中剩余资源)
  ├─ vmaDestroyAllocator           (释放所有 VMA 管理的资源)
  ├─ vkDestroyDescriptorSetLayout  (清理所有描述符集布局)
  ├─ vkDestroyDescriptorPool
  ├─ vkDestroySemaphore            (清理 Timeline Semaphore 池)
  ├─ vkDestroyCommandPool          (Graphics + Compute)
  ├─ vkDestroyDevice
  ├─ vkDestroySurfaceKHR
  └─ vkDestroyInstance
```

### 3.2 `VulkanBuffer` — 缓冲区实现

**关键设计决策**：

1. **VMA 统一管理**：使用 `vmaCreateBuffer` 一步完成 Buffer + Memory 创建绑定
2. **持久映射**：通过 `VMA_ALLOCATION_CREATE_MAPPED_BIT` 实现 CPU 端直接读写，避免每次 Map/Unmap 的驱动开销
3. **Coherent 检测**：查询 `VkMemoryPropertyFlags` 判断是否需要手动 `vkFlushMappedMemoryRanges`
4. **DeviceAddress**：所有缓冲默认开启 `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`，支持 bindless 和 GPU 端地址访问
5. **Map/Unmap 语义**：
   - `Map()` = `vkInvalidateMappedMemoryRanges`（GPU→CPU 回读，coherent 内存上为 no-op）
   - `Unmap()` = `vkFlushMappedMemoryRanges`（CPU→GPU 写入，coherent 内存上为 no-op）
6. **初始数据上传**：构造时直接 `memcpy` 到持久映射指针，非 coherent 时手动 flush

**BufferUsage → VkBufferUsageFlags 映射**：

| RHI BufferUsage | Vulkan Flags |
|-----------------|--------------|
| Vertex | `VERTEX_BUFFER_BIT` |
| Index | `INDEX_BUFFER_BIT` |
| Uniform | `UNIFORM_BUFFER_BIT` |
| Storage | `STORAGE_BUFFER_BIT` |
| Indirect | `INDIRECT_BUFFER_BIT` |
| AccelerationStruct | `ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR` |
| TransferSrc | `TRANSFER_SRC_BIT` |
| ShaderBindingTable | `SHADER_BINDING_TABLE_BIT_KHR`（RT SBT 专用） |
| (默认) | `SHADER_DEVICE_ADDRESS_BIT \| TRANSFER_DST_BIT` |

### 3.3 `VulkanTexture` — 纹理实现

**创建流程**：

1. **VMA 创建 Image**
   - `VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE`（优先 GPU 本地内存）
   - `VK_IMAGE_TILING_OPTIMAL`
   - `VK_SHARING_MODE_EXCLUSIVE`
   - 初始布局：`VK_IMAGE_LAYOUT_UNDEFINED`
   - Cubemap 标志：`VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT`
   - MSAA 采样数：支持 1/2/4/8/16/32/64x

2. **初始数据上传**（如果提供 `initialData`）：
   - 创建 CPU-visible staging buffer（`VMA_MEMORY_USAGE_CPU_ONLY`）
   - 分配一次性 Primary 命令缓冲
   - 布局转换：`UNDEFINED → TRANSFER_DST_OPTIMAL`
   - `vkCmdCopyBufferToImage` 拷贝 level 0
   - **自动 Mipmap 生成**（如果 `mipLevels > 1`）：
     ```
     Level 0: TRANSFER_DST → TRANSFER_SRC → [Blit→Level1] → SHADER_READ_ONLY
     Level 1: TRANSFER_DST → TRANSFER_SRC → [Blit→Level2] → SHADER_READ_ONLY
     ...
     Level N-1: TRANSFER_DST → SHADER_READ_ONLY (最后一级)
     ```
   - `vkQueueSubmit` + `vkQueueWaitIdle`
   - 清理 staging buffer + 临时命令缓冲

3. **ImageView 创建**
   - 自动推断 viewType：3D / Cubemap / 2D_ARRAY / 2D
   - 自动选择 aspectMask：COLOR / DEPTH / DEPTH+STENCIL

4. **Cubemap 逐面视图**
   - 额外创建 6 个 `VK_IMAGE_VIEW_TYPE_2D` 视图
   - `baseArrayLayer = 0..5`，用于离屏渲染到每个面

**TextureUsage → VkImageUsageFlags 映射**：

| RHI TextureUsage | Vulkan Flags |
|------------------|--------------|
| ShaderResource | `SAMPLED_BIT` |
| RenderTarget | `COLOR_ATTACHMENT_BIT` |
| DepthStencil | `DEPTH_STENCIL_ATTACHMENT_BIT` |
| UnorderedAccess | `STORAGE_BIT` |
| TransferSrc | `TRANSFER_SRC_BIT` |
| TransferDst | `TRANSFER_DST_BIT` |

**格式支持范围**：
- 8-bit 颜色：R8/RG8/RGBA8/BGRA8 (UNORM + SRGB)
- 16-bit 浮点：R16/RG16/RGBA16
- 32-bit 浮点：R32/RG32/RGB32/RGBA32
- 整数：R32_UINT
- 特殊：R11G11B10_FLOAT / A2B10G10R10_UNORM_PACK32（HDR10）
- 深度模板：D16_UNORM / D32_FLOAT / D24_UNORM_S8_UINT / D32_FLOAT_S8_UINT
- BC 压缩：BC1 / BC3 / BC4 / BC5 / BC7

### 3.4 `VulkanSampler` — 采样器实现

完整映射 RHI 采样器参数到 Vulkan：
- **过滤模式**：Nearest / Linear（min / mag / mip 独立）
- **寻址模式**：Repeat / MirroredRepeat / ClampToEdge / ClampToBorder
- **各向异性过滤**：`maxAnisotropy > 1` 时启用，取值来自 `SamplerDesc::maxAnisotropy`（设备上限见 `DeviceCaps::maxSamplerAnisotropy`）
- **深度比较**：`enableCompare=true` 时设置 `compareEnable + compareOp`（用于阴影贴图采样）
- **LOD 范围**：mipLodBias / minLod / maxLod

### 3.5 `VulkanSwapChain` — 交换链

**完整生命周期**：

```
构造函数 → CreateSwapchain()
  ├─ 查询表面能力 (caps / formats / presentModes)
  ├─ 格式选择: HDR 时优先 A2B10G10R10_UNORM_PACK32 + HDR10_ST.2084，否则 BGRA8_UNORM + SRGB_NONLINEAR
  ├─ 呈现模式: 优先 MAILBOX (低延迟三重缓冲), 回退 FIFO (VSync)
  ├─ 尺寸钳制: clamp(width/height, minImageExtent, maxImageExtent)
  ├─ 图像计数: max(3, caps.minImageCount), 不超过 caps.maxImageCount
  ├─ vkCreateSwapchainKHR
  │   ├─ imageUsage: COLOR_ATTACHMENT | STORAGE | TRANSFER_DST
  │   ├─ sharingMode: EXCLUSIVE
  │   └─ compositeAlpha: OPAQUE
  ├─ vkGetSwapchainImagesKHR → 获取图像数组
  ├─ 创建 ImageView (每图像一个)
  ├─ 创建同步原语: acquire 信号量 + acquire 栅栏按飞行帧槽位各一份 (kAcquireSlots = 3)，
  │                 render-complete 信号量按交换链图像各一份 (二进制信号量)
  └─ 创建深度缓冲纹理:
      ├─ 格式: D32_SFLOAT
      ├─ 分配: 手动查 Device Local 内存类型
      └─ 绑定 + 创建 ImageView

析构 → DestroySwapchain() (逆序销毁所有资源)

Resize(width, height)
  ├─ 尺寸为 0: 标记最小化, 跳过重建
  ├─ vkDeviceWaitIdle
  └─ DestroySwapchain → CreateSwapchain

AcquireNextImage()
  └─ 轮转 acquire 槽位，等待该槽位上"上次提交的消费栅栏"与 acquire 栅栏
  └─ vkAcquireNextImageKHR(UINT64_MAX, ImageAcquired[slot], AcquireFence[slot])
  └─ 返回: SUCCESS 或 SUBOPTIMAL

Present()
  └─ vkQueuePresentKHR(swapchain, imageIndex, waitSemaphore=RenderComplete)
```

### 3.6 `VulkanCommandList` — 命令缓冲核心

#### 多帧缓冲架构

每个 `VulkanCommandList` 持有 **3 套资源**（`kMaxFramesInFlight = 3`）：

```
m_CmdPools[3]        // 三缓冲命令池 (每帧独立 Reset)
m_CmdBuffers[3]      // 三缓冲命令缓冲 (Primary)
m_Fences[3]          // 三缓冲 CPU-GPU 栅栏 (初始状态 SIGNALED)
```

**Begin() 循环**：
```
Frame N:
  1. vkWaitForFences(Fences[N])         ← 等待 GPU 完成当前槽位
  2. 推进设备级延迟销毁队列              ← AdvanceFrame + AdvanceDeferredDestroy（内部按帧 ID 去重）
  3. 条件清理旧 Framebuffer             ← 如果 RenderPass 变化
  4. vkResetCommandPool(CmdPools[N])    ← 全池重置
  5. vkBeginCommandBuffer(CmdBuffers[N])
  6. m_IsRecording = true
```

**End() 循环**：
```
  1. vkEndCommandBuffer(CmdBuffers[N])
  2. m_IsRecording = false
  3. 预备 Submit (由外部驱动)
```

#### 辅助命令缓冲（Secondary CB）

- 预分配 `kMaxSecondaryCBs = 3` 个 SecondaryCB
- 通过 `BeginSecondary(PSO)` 设置继承信息（RenderPass + Subpass）
- `ExecuteSecondary(secondary)` 调用 `vkCmdExecuteCommands` 执行
- 编号递增轮转，避免同一 SecondaryCB 复用时的数据竞争

#### 渲染通道管理

**SwapChain 渲染** (`BeginRenderPass`):
- 根据 `LoadOp` 选择 RenderPass：
  - `LoadOp::Clear` → 使用 PSO 的 RenderPass（每次清屏）
  - `LoadOp::Load` → 懒创建专用 `m_LoadRenderPass`（保留 BackBuffer 内容）
    - 颜色附件：`initialLayout=PRESENT_SRC → finalLayout=PRESENT_SRC`
    - 深度附件：`initialLayout=DEPTH_STENCIL_READ_ONLY → finalLayout=DEPTH_STENCIL_READ_ONLY`
- RenderPass 变化时强制重建 Framebuffer
- 清除值：颜色 (默认深蓝灰) + 深度 (1.0) + 模板 (0)

**离屏渲染** (`BeginOffscreenPass`):
- 接受 `void*` 类型的 VkImageView 句柄（跨模块传递）
- 支持深度专用通道（`colorImageView = nullptr`，用于阴影贴图绘制）
- 临时创建 Framebuffer（`m_CurrentOffscreenFB`），在 `EndOffscreenPass` 时交给设备级延迟销毁队列
  `VulkanDevice::GetDeferredDestroy()`（6 槽环形缓冲，kSlots = 2 × kMaxFramesInFlight）

**MRT 离屏渲染** (`BeginOffscreenPassMRT`):
- 最多支持 **8 个颜色附件 + 1 个深度附件**（共 9 个附件）
- 清除值数组契约：`colorCount` 个颜色项 + 末尾 1 个深度项（`clears[colorCount]`），最多 9 个元素
- SecondaryCB 内联执行标志：`VK_SUBPASS_CONTENTS_INLINE_AND_SECONDARY_COMMAND_BUFFERS_KHR`（需 VK_KHR_maintenance7）

#### Barrier 系统

三个核心辅助函数将 RHI 抽象映射到 Vulkan：

```
ToVkImageLayout(ResourceState)
  - 位掩码匹配，写入状态优先
  - 优先级: DepthStencilWrite > DepthStencilRead > RenderTarget > ShaderResource > Present > UnorderedAccess > CopySrc > CopyDst

ToVkPipelineStageFlags(PipelineStage)
  - 位掩码转换: ColorAttachmentOutput/FragmentShader/ComputeShader/Transfer/
    EarlyFragmentTests/LateFragmentTests/VertexShader/RayTracingShader/ASBuild
  - 空掩码回退到 ALL_COMMANDS_BIT

ToVkAccessFlags(ResourceState)
  - 位掩码转换: DepthStencilWrite→ATTACHMENT_WRITE, ShaderResource→SHADER_READ,
    RenderTarget→COLOR_ATTACHMENT_WRITE, UnorderedAccess→SHADER_WRITE,
    CopySrc→TRANSFER_READ, CopyDst→TRANSFER_WRITE
  - 空掩码回退到 MEMORY_READ | MEMORY_WRITE
```

支持两种 Barrier 重载：
1. **全局内存屏障**：`VkMemoryBarrier`，无图像
2. **图像布局转换**：`VkImageMemoryBarrier`，自动选择 Color/Depth aspect，覆盖所有 mip 和 array layer

#### 跨队列所有权转移

支持 `QueueOwnershipTransfer` 两种重载（纹理 + 缓冲）：
- 在 Barrier 中设置 `srcQueueFamilyIndex ≠ dstQueueFamilyIndex`
- 实现 Vulkan 队列族所有权 Release-Acquire 语义
- Aspect mask 自动根据纹理格式选择（Color / Depth）

便捷方法：
- `ReleaseToQueue(texture, dstQueue)` — 从当前队列释放到目标
- `AcquireFromQueue(texture, srcQueue)` — 从源队列获取到当前队列

#### Submit 与 Timeline Semaphore 集成

**Submit 流程**：
```
Submit()
  ├─ 收集二进制信号量 (SwapChain):
  │   ├─ Wait: ImageAcquired (图像可用)
  │   └─ Signal: RenderComplete (渲染完成)
  ├─ 收集 Timeline 信号量 (跨队列同步):
  │   ├─ Wait: m_TimelineWaitSem + value (等待上游队列完成)
  │   └─ Signal: m_TimelineSignalSem + value (通知下游队列)
  ├─ 构建 VkTimelineSemaphoreSubmitInfo:
  │   ├─ waitSemaphoreValueCount + pWaitSemaphoreValues
  │   └─ signalSemaphoreValueCount + pSignalSemaphoreValues
  ├─ vkResetFences(当前帧 fence)
  ├─ vkQueueSubmit(queue, submitInfo, fence)
  ├─ 清除 Timeline 状态 (Signal/Wait 单次生效)
  └─ m_FrameIndex = (m_FrameIndex + 1) % 3
```

**信号量数组结构**：
```
Wait 信号量:  [二进制 ImageAcquired] + [Timeline Wait]
Signal 信号量: [二进制 RenderComplete] + [Timeline Signal]
```

#### PushConstants 阶段自动适配

```cpp
SetPushConstants(offset, size, data):
  if (Compute PSO)    → VK_SHADER_STAGE_COMPUTE_BIT
  if (RT PSO)         → RAYGEN | MISS | CLOSEST_HIT | ANY_HIT | CALLABLE
  else (Graphics PSO) → VERTEX | FRAGMENT
```

### 3.7 `VulkanPipeline` — 管线状态创建

工厂函数 `CreateVulkanPipeline(device, desc, descLayouts, vulkanDevice)` 支持三种管线类型的分派创建。
入口处先按 `PipelineStateDesc` 哈希查 PSO 缓存（命中则直接复用底层 Vulkan 对象）；图形管线在设备支持
`VK_EXT_graphics_pipeline_library` 且非 `indirectBindable` 时走 GPL 四段库 + fast-link 路径：

#### 类型 1: Compute Pipeline

```
条件: desc.bindPoint == PipelineBindPoint::Compute
流程:
  1. vkCreateShaderModule (COMPUTE_BIT)
  2. vkCreatePipelineLayout (DescriptorSetLayouts + PushConstantRanges)
  3. vkCreateComputePipelines (单阶段，无 RenderPass)
  4. vkDestroyShaderModule
返回: VulkanPipelineState(device, pipeline, layout, VK_NULL_HANDLE, COMPUTE)
```

#### 类型 2: Mesh Shader Pipeline

```
条件: desc.meshShader != nullptr && !meshShader->spirv.empty()
着色器阶段: [Task(可选)] → Mesh → [Fragment(可选)]
关键差异:
  - 无 IA 阶段 (vertexInput 为空绑定)
  - 无 Vertex Input (Mesh Shader 自行 fetch 顶点)
  - topology 固定 TRIANGLE_LIST (由 Mesh Shader outputtopology 决定)
  - 仍然创建 RenderPass (颜色 + 深度附件)
返回: VulkanPipelineState(device, pipeline, layout, renderPass, GRAPHICS)
```

#### 类型 3: 传统 Graphics Pipeline

```
条件: 默认 (Vertex + Fragment)
流程:
  1. vkCreateShaderModule (VERTEX_BIT + FRAGMENT_BIT)
  2. vkCreateRenderPass (MRT: 最多 8 颜色附件 + 1 深度附件)
     ├─ BGRA8 / HDR10(A2B10G10R10) → finalLayout = PRESENT_SRC_KHR
     └─ 其他 → finalLayout = COLOR_ATTACHMENT_OPTIMAL
     └─ 深度 → finalLayout = DEPTH_STENCIL_READ_ONLY_OPTIMAL
     └─ SubpassDependency: EXTERNAL → subpass 0
  3. VertexInputState: 从 VertexInputLayout 构建属性列表 + 绑定步长
     └─ 回退: 无属性时根据 stride 推导默认格式
  4. 标准管线状态:
     ├─ InputAssembly: TRIANGLE_LIST（当前固定，不随 desc.topology 变化）
     ├─ ViewportState: 动态状态 (VK_DYNAMIC_STATE_VIEWPORT | SCISSOR)
     ├─ Rasterization: 可配置 cullMode / frontFace / fillMode（默认 CullMode::None、
     │                 FrontFace::Clockwise、FillMode::Solid）
     ├─ Multisample: SAMPLE_COUNT_1_BIT（固定）
     ├─ DepthStencil: 可配置 depthTest/depthWrite/depthCompare；depthBounds/stencil 关闭
     └─ ColorBlend: 每 MRT 从 desc.colorBlend[c] 构建（blendEnable/因子/操作/写掩码）
  5. vkCreatePipelineLayout (DescriptorSetLayouts + PushConstantRanges)
  6. vkCreateGraphicsPipelines
  7. 清理 ShaderModule
返回: VulkanPipelineState(device, pipeline, layout, renderPass, GRAPHICS)
```

### 3.8 描述符集系统

#### Descriptor Pool 预分配容量

```
VkDescriptorPoolSize[] poolSizes:
  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:                64
  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:             16384   ← SSBO + bindless SSBO 数组
  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:      8192   ← 阴影贴图 / IBL 等非 bindless 组合采样器
  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:              32768   ← bindless 纹理数组
  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:                256
  VK_DESCRIPTOR_TYPE_SAMPLER:                    32768   ← bindless 采样器数组
  VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:    64   ← RT TLAS 绑定
maxSets: 1024
flags: FREE_DESCRIPTOR_SET_BIT | UPDATE_AFTER_BIND_BIT
```

#### Bindless 支持

所有 binding 默认带 `VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT`（允许未绑定访问），
并带 `VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT`（例外：加速结构绑定在设备未提供
`descriptorBindingAccelerationStructureUpdateAfterBind` 时不带该位）。
Bindless binding 额外使用：
- `VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT`（可变数量；**只能**设在 binding 号最大的
  bindless 绑定上，不满足时退回按声明数量满额分配）

分配时通过 `VkDescriptorSetVariableDescriptorCountAllocateInfo` 指定最大可变计数。

#### UpdateDescriptorSet 五种重载

| 重载 | DescriptorType | 用途 |
|------|---------------|------|
| Buffer | UniformBuffer / StorageBuffer | 单缓冲绑定（VkDescriptorBufferInfo） |
| Texture+Sampler (单) | CombinedImageSampler | 单个组合图像采样器 |
| Texture+Sampler (数组) | SampledImage / Sampler / CombinedImageSampler | Bindless 纹理/采样器数组绑定 |
| Buffer (数组) | StorageBuffer | Bindless SSBO 数组绑定 |
| AccelerationStructure | AccelerationStructure | RT PSO 的 TLAS 绑定 |

另有独立方法 `UpdateDescriptorSetWithImageView`（StorageImage）：直接绑定 ImageView，用于
SwapChain BackBuffer 等非 `IRHITexture` 图像。

#### 非法句柄检测

`UpdateDescriptorSet(单纹理)` 中包含运行时句柄检查：
```cpp
// 1) 野指针检测：纹理已销毁（VulkanTextureLiveness 登记）
if (!IsTextureAlive(texture)) { HE_CORE_ERROR("... 野指针 ..."); return; }
// 2) imageView 非法（空句柄 / 0xdddddddddddddddd）
if (imgView == VK_NULL_HANDLE || imgView == (VkImageView)0xdddddddddddddddd) {
    HE_CORE_ERROR("UpdateDescriptorSet: 非法 imageView! ...");
    return;
}
// 3) CombinedImageSampler 必须提供有效采样器（SampledImage 允许为空）
if (vkType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
    (vkSamp == VK_NULL_HANDLE || vkSamp == (VkSampler)0xdddddddddddddddd)) { ... return; }
```
检测 `0xdddddddddddddddd`（MSVC Debug 内存填充模式）防止使用已释放的句柄；另有
「描述符指向一张从未被写入的纹理」的告警检查。

#### Per-Mip ImageView 支持

用于 Hi-Z 金字塔构建等需要写入特定 mip level 的场景：
- `CreateTextureMipStorageView(texture, mipLevel)` — 创建 `VK_IMAGE_VIEW_TYPE_2D` 视图，限定单个 mipLevel（layer 0）
- `CreateTextureMipStorageView(texture, mipLevel, arrayLayer)` — 追加逐层限定（Cubemap 逐面渲染）
- `CreateTextureMipSampledView(texture, mipLevel)` / `(texture, mipLevel, arrayLayer)` — 同上逻辑，用于采样单个 mip（/层）
- `DestroyTextureMipView(view)` — 销毁上述视图

### 3.9 Timeline Semaphore — 跨队列同步

RHI 的 `RHIFenceHandle` 实际上封装了 **Vulkan Timeline Semaphore**（而非传统 VkFence）：

```
CreateFence()
  → vkCreateSemaphore(TIMELINE, initialValue=0)
  → 存入 m_Fences 向量
  → 返回: (索引+1) 作为 u64 句柄

DestroyFence(handle)
  → vkDestroySemaphore

WaitForFence(handle, value, timeout)
  → vkWaitSemaphores(device, &waitInfo, timeout)
  → 返回: 是否 VK_SUCCESS

GetFenceValue(handle)
  → vkGetSemaphoreCounterValue

SignalFenceOnQueue(queue, handle, value)
  → vkQueueSubmit(空提交) + VkTimelineSemaphoreSubmitInfo(signal)

WaitFenceOnQueue(queue, handle, value)
  → vkQueueSubmit(空提交) + VkTimelineSemaphoreSubmitInfo(wait)
  → waitStage = ALL_COMMANDS_BIT
```

**SubmitAll 批量提交**：
```cpp
SubmitAll(cmdLists):
  1. 按 QueueType 分组 (Graphics vs Compute)
  2. 先提交 Graphics 组，再提交 Compute 组
  // 目的：确保 Compute 命令在 Graphics 之后执行（隐式排序）
```

### 3.10 Ray Tracing 支持

#### 加速结构 (`VulkanAccelerationStructure`)

**BLAS/TLAS 创建流程**：
```
1. VMA 创建底层存储缓冲
   usage: ACCELERATION_STRUCTURE_STORAGE_BIT | SHADER_DEVICE_ADDRESS_BIT
   memory: VMA_MEMORY_USAGE_AUTO

2. vkCreateAccelerationStructureKHR(type=BLAS|TLAS, buffer, size)
   → m_AS

3. vkGetAccelerationStructureDeviceAddressKHR(device, &asAddrInfo)
   → m_DeviceAddress (GPU 地址，用于 TLAS 实例引用)

4. (BLAS) 存储构建描述 → m_BLASDesc (用于后续 BuildBLAS 调用)
```

**BLAS 构建** (`BuildBLAS`):
- 遍历 `BLASBuildDesc::geometries`，构建 `VkAccelerationStructureGeometryKHR` 数组
- 三角形几何：vertexBuffer + indexBuffer + (可选) transformBuffer 的 GPU 地址
- 使用 `VK_GEOMETRY_OPAQUE_BIT_KHR` 标志
- Build/Update 双模式
- `rt.cmdBuildAS(cb, count, pInfos, ppRangeInfos)` 执行

**TLAS 构建** (`BuildTLAS`):
- 使用 `VK_GEOMETRY_TYPE_INSTANCES_KHR` 几何类型
- 实例缓冲提供 GPU 地址
- 构建后自动插入 barrier：`AS_WRITE → AS_READ`（阶段：`AS_BUILD → RAY_TRACING_SHADER`）

#### RT Pipeline State (`VulkanRTPipelineState`)

**着色器组类型映射**：
```
RTShaderGroupType → VkRayTracingShaderGroupTypeKHR:
  RayGen   → GENERAL
  Miss     → GENERAL
  Hit      → TRIANGLES_HIT_GROUP
  Callable → GENERAL
```

**管线创建** (`CreateRTPipelineState`):
```
1. 创建所有 ShaderModule (根据 ShaderStage 选择 VkShaderStageFlagBits)
2. 构建 VkRayTracingShaderGroupCreateInfoKHR 数组
3. vkCreatePipelineLayout (DescriptorSetLayouts + PushConstantRanges)
4. m_RT.createRTPipelines(device, VK_NULL_HANDLE, m_PipelineCache, 1, &rtInfo, nullptr, &pipeline)
5. m_RT.getRTShaderGroupHandles(device, pipeline, 0, groupCount, handleDataSize, handles.data())
   → 获取着器组句柄数据（用于 SBT 填充）
6. 清理 ShaderModule
```

#### TraceRays — SBT 地址计算

通过 `VkStridedDeviceAddressRegionKHR` 描述 4 个 SBT 槽位：
```
SBT Buffer GPU 地址 + 各 SBT 槽位偏移:
  RayGen Region:   baseAddr + sbt.rayGen.handleOffset
  Miss Region:     baseAddr + sbt.miss.handleOffset
  Hit Region:      baseAddr + sbt.hit.handleOffset
  Callable Region: baseAddr + sbt.callable.handleOffset

自动计算各 Region 的 size（通过排序 offset 推断边界）
```

### 3.11 Device Generated Commands (DGC)

`VulkanDGC` 类封装 `VK_EXT_device_generated_commands`：

**初始化流程** (`Initialize`):
```
1. 创建 IndirectCommandsLayout
   - 两个令牌：EXECUTION_SET_EXT（必须首位）+ DRAW_INDEXED_EXT（必须末位）
   - indirectStride = sizeof(u32) * 5 (VkDrawIndexedIndirectCommand)
   - shaderStages = VERTEX | FRAGMENT
   - pipelineLayout = VK_NULL_HANDLE (管线布局由执行集提供，无 PushConstant 令牌)

2. 创建 IndirectExecutionSet
   - type: PIPELINES
   - initialPipeline: GBuffer PSO
   - maxPipelineCount: 1

3. 查询预处理缓冲大小
   - vkGetGeneratedCommandsMemoryRequirementsEXT
   - 如果大小为 0: 驱动无需预处理，直接标记完成

4. 创建预处理缓冲 (Device Local + BDA)
   - 手动查找 Device Local 内存类型
   - vkCreateBuffer + vkAllocateMemory + vkBindBufferMemory
   - vkGetBufferDeviceAddress → m_PreprocessAddress
```

**执行路径**：
```
VulkanCommandList::ExecuteGeneratedCommands(desc):
  → 构建 VkGeneratedCommandsInfoEXT:
    - indirectCommandsLayout / indirectExecutionSet ← VulkanDGC
    - indirectAddress = desc.sequencesBufferAddr
    - preprocessAddress = desc.preprocessBufferAddr
    - sequenceCountAddress = desc.sequenceCountAddr
    - maxSequenceCount / maxDrawCount
  → m_VulkanDevice->GetDGCFuncs().vkCmdExecuteGeneratedCommandsEXT(cb, VK_FALSE, &genInfo)
```

### 3.12 GPU 时间戳查询

`VulkanQueryPool` 支持两类查询池：`VK_QUERY_TYPE_TIMESTAMP`（默认）与
`VK_QUERY_TYPE_PIPELINE_STATISTICS`（6 个计数器：IA 顶点/图元、VS 调用、裁剪调用/图元、FS 调用）：

```
CreateQueryPool(count, type = Timestamp):
  → vkCreateQueryPool(device, {type, count, [pipelineStatistics]}, &m_Pool)

WriteTimestamp(pool, index):
  → vkCmdWriteTimestamp(BOTTOM_OF_PIPE, pool, index)

ResetQueryPool(pool):
  → vkCmdResetQueryPool(cb, pool, 0, count)

GetQueryResults(pool, first, count, data):
  → vkGetQueryPoolResults(device, pool, first, count, size, data, stride=8,
       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT)

TryGetQueryResults(pool, first, count, data):
  → vkGetQueryPoolResults(... VK_QUERY_RESULT_WITH_AVAILABILITY_BIT, stride=16)
  → 非阻塞：结果未就绪时返回 false（避免多命令列表下的死等）
  → BeginQuery / EndQuery 对应 vkCmdBeginQuery / vkCmdEndQuery

GetTimestampPeriod():
  → VkPhysicalDeviceProperties::limits.timestampPeriod (纳秒)
```

### 3.13 格式转换映射表

**Format → VkFormat** 涵盖以下分类（`Format` 枚举中除 Unknown/Count 外的 27 项全部有映射，
缺项会静默回落到 `VK_FORMAT_UNDEFINED`，不报编译错误）：
- **8-bit 颜色**: R8/RG8/RGBA8/BGRA8 (UNORM + SRGB) × 8
- **16-bit 浮点**: R16/RG16/RGBA16 × 3
- **32-bit 浮点**: R32/RG32/RGB32/RGBA32 × 4
- **整数**: R32_UINT
- **特殊**: R11G11B10_FLOAT / A2B10G10R10_UNORM_PACK32 (HDR10)
- **深度模板**: D16/D32/D24S8/D32S8 × 4
- **BC 压缩**: BC1/BC3/BC4/BC5/BC7 × 5

**FilterMode → VkFilter**:
```
Nearest → VK_FILTER_NEAREST
Linear  → VK_FILTER_LINEAR
```

**AddressMode → VkSamplerAddressMode**:
```
Repeat         → REPEAT
MirroredRepeat → MIRRORED_REPEAT
ClampToEdge    → CLAMP_TO_EDGE
ClampToBorder  → CLAMP_TO_BORDER
```

**CompareFunc → VkCompareOp** (完整映射 8 种比较函数)

### 3.14 `VulkanDeviceAccess` — 内部桥接

静态方法桥接 `IRHIDevice*` → `VulkanDevice*`，供引擎内部模块在需要直接访问 Vulkan 句柄时使用（如 RenderGraph 的资源转换）：

```cpp
struct VulkanDeviceAccess {
    static VkInstance       GetInstance(IRHIDevice* d);
    static VkPhysicalDevice GetPhysical(IRHIDevice* d);
    static VkDevice         GetDevice(IRHIDevice* d);
    static u32              GetGraphicsFamily(IRHIDevice* d);
    static VkQueue          GetGraphicsQueue(IRHIDevice* d);
    static VkCommandPool    GetGraphicsCmdPool(IRHIDevice* d);
    static VkQueue          GetComputeQueue(IRHIDevice* d);
    static u32              GetComputeFamily(IRHIDevice* d);
};
```

实现即 `static_cast<VulkanDevice*>(d)->GetVkXxx()`。

---

## 四、架构模式总结

### 4.1 设计模式与实现手法

| 模式 | 实现位置 | 说明 |
|------|----------|------|
| **工厂模式** | `IRHIDevice` | 所有 GPU 资源由 Device 统一创建 |
| **策略模式** | `CreateDevice(Backend)` | 按后端类型实例化不同的 Device 子类 |
| **不透明句柄** | `Types.h` | `DescriptorSetLayoutHandle` 等用 `u64` 索引而非指针，跨后端安全 |
| **单例访问** | `g_Device` | 全局访问当前设备，避免逐层传递 |
| **三缓冲** | `VulkanCommandList` | 3 帧环形命令缓冲 + Fence 同步，CPU/GPU 并行 |
| **延迟销毁** | `DeferredDestructionQueue` | 设备级 6 槽环形队列，离屏 FB/PSO 等延迟 kMaxFramesInFlight 帧销毁（`m_PendingFBs` 已移除） |
| **扩展条件加载** | `VulkanDevice` | RT/Mesh/DGC 函数指针按硬件能力条件加载 |
| **编译单元拆分** | 多个 `.cpp` 文件 | VulkanDevice / VulkanCommandList 按功能拆分为多个编译单元 |
| **跨编译单元符号共享** | `VulkanConverters.h` / `VulkanRT.h` | `ToVkFormat` / `ToVkCompareOp` 声明在 `VulkanConverters.h`（实现在同名 cpp）；`ToVkBuildFlags` 声明在 `VulkanRT.h`（实现在 `VulkanDevice.cpp`） |

### 4.2 Vulkan 特定优化

| 优化 | 说明 |
|------|------|
| **VMA 全局管理** | 所有 Buffer/Image/AS 内存通过 VMA 统一分配，避免手动内存类型选择 |
| **持久映射** | CPU 可见缓冲使用 `VMA_ALLOCATION_CREATE_MAPPED_BIT`，减少 Map/Unmap 驱动调用 |
| **Descriptor Indexing** | 全面使用 `UPDATE_AFTER_BIND` + bindless，描述符池中 SampledImage / Sampler 各 32768（bindless 纹理与采样器数组各占一个 set 的额度）|
| **Timeline Semaphore** | 统一跨队列同步模型，Signal/Wait 值单调递增，支持 CPU 等待和 GPU-GPU 等待 |
| **Mailbox 呈现** | 优先低延迟三重缓冲，减少输入延迟 |
| **Buffer Device Address** | 所有缓冲默认开启 BDA，GPU 端可访问任意缓冲地址 |
| **动态状态** | Viewport + Scissor 使用动态状态，避免 PSO 重组 |

### 4.3 当前功能覆盖范围

| 功能 | 状态 | 说明 |
|------|------|------|
| 基础 Graphics/Compute 管线 | ✅ | Vertex+Fragment / Compute PSO |
| MRT 离屏渲染 (Deferred) | ✅ | 最多 8 个颜色附件 + 1 个深度附件 |
| 阴影贴图 (Depth-only Pass) | ✅ | 无颜色附件的离屏通道 |
| Cubemap 逐面渲染 | ✅ | 6 个逐面 2D ImageView |
| Bindless 纹理数组 | ✅ | Descriptor Indexing + Variable Count |
| Ray Tracing | ✅ | BLAS/TLAS + RTPSO + SBT |
| Mesh Shader | ✅ | Task + Mesh + Fragment 管线 |
| AsyncCompute | ✅ | 三级队列检测 (Tier 0/1/2) |
| DGC (GPU Driven) | ✅ | VK_EXT_device_generated_commands |
| GPU 时间戳查询 | ✅ | VK_QUERY_TYPE_TIMESTAMP |
| Timeline Semaphore 同步 | ✅ | 跨队列 + CPU-GPU 等待 |
| Secondary Command Buffer | ✅ | 多线程并行录制 |
| 纹理 Mipmap 生成 | ✅ | 逐级 Blit + Barrier |
| 深度比较采样器 | ✅ | 阴影贴图 PCF 采样 |
| BC1-7 压缩纹理 | ✅ | 支持初始数据上传 |
| LoadOp::Load (BackBuffer 保留) | ✅ | 懒创建 LOAD RenderPass |
| PSO 缓存 | ✅ | 内存哈希复用 + `VkPipelineCache` 磁盘持久化（pipeline_cache.bin） |
| Graphics Pipeline Library | ✅ | VK_EXT_graphics_pipeline_library 四段库 + fast-link |
| PSO 后台预热 / 创建限流 | ⚠️ | 注册与逐帧限流队列已接入；后台预热线程的启动当前在 `DeferredPipeline` 中被注释掉 |
| 瞬态资源配置 | ✅ | TransientResourceAllocator（128MB × 2 Heap + VkImage 缓存），RenderGraph 自动走此路径 |
| 管线统计查询 | ✅ | VK_QUERY_TYPE_PIPELINE_STATISTICS（IA/VS/裁剪/FS 共 6 个计数器） |
| Debug Label / DrawCall Marker | ✅ | VK_EXT_debug_utils（Begin/End/Insert + `SetDrawDebugLabel`，受 `r.Debug.DrawMarker` 控制） |
| 资源调试命名 | ✅ | `SetResourceDebugName` 覆盖 Buffer/Texture/PSO/Sampler/AS |
| 纹理 → 缓冲读回 | ✅ | `CopyTextureToBuffer`（GPU 端拷贝，供数值校验/读回） |
| HDR10 交换链 | ✅ | `SwapChainDesc::hdr` → A2B10G10R10_UNORM_PACK32 + ST.2084 |

### 4.4 待实现

| 功能 | 说明 |
|------|------|
| D3D12 后端 | 当前仅 `Backend::D3D12` 枚举占位（无任何实现文件），需要 D3D12 实现类 |
| Metal 后端 | 当前仅 `Backend::Metal` 枚举占位，macOS/iOS 平台支持 |
| WebGPU 后端 | 当前仅 `Backend::WebGPU` 枚举占位，浏览器平台支持 |
| Transfer Queue 专用 | 当前 Copy 队列回退到 Graphics |
| VRS (可变速率着色) | DeviceCaps 中已有字段 |
| Work Graphs | DeviceCaps 中已有字段 |
| OMM (Opacity Micromaps) | DeviceCaps 中已有字段 |
| SER (Shader Execution Reordering) | DeviceCaps 中已有字段 |
