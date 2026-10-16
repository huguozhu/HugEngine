# Vulkan 名词大全（含义 · 作用 · 代码调用）

> 范围：Vulkan 核心与常用扩展的名词梳理，每个名词给 **含义 / 作用 / 原生 API / 本引擎 RHI 调用**
> 口径说明：
> · **原生 API 列**是 Vulkan 标准函数名（常识性内容，不逐条给出处）；
> · **引擎列**只列**本会话实测**到的调用（`文件:行号` 见各文档）；**未列出的不等于不存在** ✗，
>   只是本文没有核实过；
> · 相关文档：[Vulkan_D3D11_D3D12_Metal主要名称对应.md](Vulkan_D3D11_D3D12_Metal主要名称对应.md)、
>   [Vulkan渲染一个物体的典型步骤.md](Vulkan渲染一个物体的典型步骤.md)

---

<!-- TOC: auto-generated -->

## 目录

- [1. 实例 / 设备 / 内存](#1-实例-设备-内存)
- [2. 呈现链（窗口）](#2-呈现链窗口)
- [3. 资源](#3-资源)
- [4. 描述符与常量](#4-描述符与常量)
- [5. 着色器与管线](#5-着色器与管线)
- [6. 命令 / 渲染 / 同步](#6-命令-渲染-同步)
- [7. 光追专用](#7-光追专用)
- [8. 最容易混的 8 对](#8-最容易混的-8-对)
- [9. 与 D3D 的速查对照](#9-与-d3d-的速查对照)
- [10. 未复核清单](#10-未复核清单)
- [相关文档](#相关文档)

---

## 1. 实例 / 设备 / 内存

| 名词 | 含义 | 作用 | 原生 API | 本引擎 RHI |
|---|---|---|---|---|
| **Instance** | 应用与 loader 的连接 | 加载驱动、枚举 GPU、启用扩展/校验层 | `vkCreateInstance` / `vkDestroyInstance` | `VulkanDevice` 封装（初始化内） |
| ApplicationInfo | 应用名/API 版本 | 驱动据此选行为 | `VkApplicationInfo` | — |
| **Validation Layer** | API 用法检查层 | 调试期抓误用（VUID） | `VK_LAYER_KHRONOS_validation` | `enableValidation`（`EngineConfig`） |
| **Debug Utils** | 调试消息回调 | 把校验输出接进日志 | `vkCreateDebugUtilsMessengerEXT` | `VulkanDevice.cpp` 有 debug messenger 创建 |
| **Physical Device** | 一块物理 GPU | 查能力/限额（能力探测源头） | `vkEnumeratePhysicalDevices` / `vkGetPhysicalDeviceProperties` / `vkGetPhysicalDeviceFeatures2` / `vkGetPhysicalDeviceQueueFamilyProperties` | `VulkanDevice::SelectPhysicalDevice`、`QueryCaps()`（`caps.maxBindlessResources` 等） |
| **Logical Device** | 与物理设备的逻辑连接 | 一切资源/队列/管线都从它创建 | `vkCreateDevice` / `vkDestroyDevice` | `VulkanDevice::Initialize` |
| **Queue / Queue Family** | 队列 = 提交通道；族按能力分 | graphics / compute / transfer / sparse | `vkGetDeviceQueue` / `vkQueueSubmit` | `VulkanDevice::Submit` / `SubmitAll`；队列族检测在 `FindQueueFamilies` |
| **Extension / Feature** | 扩展与特性位 | 新能力必须**显式启用** | `VK_KHR_*` / `VK_EXT_*` + `VkPhysicalDeviceFeatures2` | `descriptorIndexing` 一族、`drawIndirectCount`、`meshShader` 等（`CreateLogicalDevice`） |
| **Device Memory / Heap / Type** | 资源与内存**分离** | Heap = 显存/系统内存池；Type 决定 device-local / host-visible / coherent | `vkAllocateMemory` / `vkGetPhysicalDeviceMemoryProperties` | `VulkanAllocator`（未逐行读） |
| **Bind（绑定）** | 资源与内存手动绑定 | 可做别名复用 | `vkBindBufferMemory` / `vkBindImageMemory` | 同上 |
| **Memory Requirements** | 对齐/大小要求 | 分配前必查 | `vkGetBufferMemoryRequirements` / `vkGetImageMemoryRequirements` | 同上 |
| **Memory Aliasing** | 多资源共用内存 | 帧图/瞬态资源核心手段 | 无专门 API（靠 bind） | `TransientResourceAllocator`（`m_TransientAllocator.Initialize(… 128MB)`） |
| **Map / Unmap** | 映射 host 可见内存 | CPU 写 GPU 数据 | `vkMapMemory` / `vkUnmapMemory` / `vkFlushMappedMemoryRanges` | `m_ObjectBuf->Map()` |

## 2. 呈现链（窗口）

| 名词 | 含义 | 作用 | 原生 API | 本引擎 RHI |
|---|---|---|---|---|
| **Surface** | 窗口系统的绘制目标 | 各平台各一个（Win32/Xlib/Wayland/Metal/**OHOS**） | `vkCreateWin32SurfaceKHR`（平台相关） | `VulkanDevice::CreateSurface(void* windowHandle)` |
| **Present 支持查询** | 队列族能否呈现 | 选队列族时要查 | `vkGetPhysicalDeviceSurfaceSupportKHR` | `FindQueueFamilies`（呈现族） |
| **Swapchain** | 一组可呈现图像的环 | present mode/图像数/格式 | `vkCreateSwapchainKHR` / `vkGetSwapchainImagesKHR` | `VulkanSwapchain` |
| **Swapchain Image** | swapchain 提供的 `VkImage` | **不由你分配** | `vkGetSwapchainImagesKHR` | 同上 |
| **Acquire / Present** | 取图 / 交回 | 与同步原语串起来 | `vkAcquireNextImageKHR` / `vkQueuePresentKHR` | 渲染线程循环（`Acquire → 录制 → Submit → 读回 → Present`） |
| **Present Mode** | 呈现节奏 | FIFO 必有；MAILBOX 低延迟 | `VkPresentModeKHR` | `VulkanSwapchain` 创建参数 |

## 3. 资源

| 名词 | 含义 | 作用 | 原生 API | 本引擎 RHI |
|---|---|---|---|---|
| **Buffer** | 线性数据 | 顶点/索引/uniform/SSBO/indirect | `vkCreateBuffer` | `device->CreateBuffer(desc)` |
| **Image** | 多维图像 | 纹理/附件；Tiling 决定内存布局 | `vkCreateImage` | `device->CreateTexture(desc)`（帧图 `rg.CreateTexture`） |
| **Image View** | 图像的使用视图 | 重解释格式/选 mip/array 范围 | `vkCreateImageView` | RHI 内封装（未逐行读） |
| **Sampler** | 过滤/寻址/mip/各向异性 | **与纹理分离**是 Vulkan 特点 | `vkCreateSampler` | `m_Lighting.GetPointSampler()` 等 |
| **Combined Image Sampler** | 两者合成一个描述符 | GLSL 的 `sampler2D` | 描述符类型 `COMBINED_IMAGE_SAMPLER` | bindless 注册时**纹理+采样器一起**（`RegisterTexture(tex, sampler)`） |
| **Buffer View** | 格式化缓冲视图 | texel buffer | `vkCreateBufferView` | — |
| **Format** | 像素/顶点格式 | 兼容性/附件支持要查 | `VkFormat` | `rhi::Format::RGBA16_FLOAT` 等 |
| **Image Layout** | 图像的**使用状态**（Vulkan 独有） | 必须靠 barrier 转换 | `VkImageLayout` + `vkCmdPipelineBarrier` | `cmd->PipelineBarrier(from, to, s0, s1, tex)` |
| **Subresource** | mip/layer/aspect 组合 | barrier/拷贝/清除的最小单位 | `VkImageSubresourceRange` | RHI 内封装 |
| **Copy** | 资源间搬运 | 上传/回读 | `vkCmdCopyBufferToImage` / `vkCmdCopyImageToBuffer` / `vkCmdUpdateBuffer` | 上传路径（未逐行读） |

## 4. 描述符与常量

| 名词 | 含义 | 作用 | 原生 API | 本引擎 RHI |
|---|---|---|---|---|
| **Descriptor Set Layout** | "这组绑定长什么样" | 声明 binding 号/类型/数量/阶段 | `vkCreateDescriptorSetLayout` | `device->CreateDescriptorSetLayout(l)` |
| **Descriptor Set** | 具体绑定的一批资源 | 按 Layout 分配与更新 | `vkAllocateDescriptorSets` / `vkUpdateDescriptorSets` | RHI 描述符集（`VulkanDevice_Descriptors.cpp`） |
| **Descriptor Pool** | 分配 Set 的池 | 池满要重建/扩容 | `vkCreateDescriptorPool` | 同上（含 ImGui 描述符池销毁） |
| **Descriptor Type** | uniform/storage buffer、sampled image、sampler、storage image、input attachment、**acceleration structure** | 决定 shader 声明 | `VkDescriptorType` | `rhi::DescriptorType::CombinedImageSampler` |
| **Push Constants** | 不经描述符的小常量 | 最快的小数据通道 | `vkCmdPushConstants` | `cmd->SetPushConstants(0, sizeof(pc), &pc)`（`PushConstantData` 锁 160 B） |
| **Bindless / Descriptor Indexing** | 无界数组 + 运行期索引 | 免逐资源重绑 | `runtimeDescriptorArray` / `partiallyBound` / `updateAfterBind` / 非一致索引 | `IRHIBindlessHeap`（`RegisterTexture/Buffer`、`Flush`、`BindlessSlotRing`） |
| **Descriptor Set 更新时机** | 立即 vs 飞行中 | 影响同步点 | `vkUpdateDescriptorSets` + `UPDATE_AFTER_BIND` | bindless 用 pending + `Flush()` 批量写 |

## 5. 着色器与管线

| 名词 | 含义 | 作用 | 原生 API | 本引擎 RHI |
|---|---|---|---|---|
| **Shader Module** | SPIR-V 字节码容器 | 只是字节码，不含状态 | `vkCreateShaderModule` | Slang→SPIR-V→内嵌头（构建期） |
| **Pipeline（Graphics）** | **完整状态对象** | 输入/装配/视口/光栅/深度/混合/多采样 | `vkCreateGraphicsPipelines` | `device->CreatePipelineState(desc)` |
| **Pipeline（Compute/RT）** | 计算 / 光追管线 | RT 含 rgen/rchit/rmiss/rahit + SBT | `vkCreateComputePipelines` / `vkCreateRayTracingPipelinesKHR` | `RTPass` 等 |
| **Pipeline Layout** | 描述符集布局 + push constant 范围 | 管线与资源接口的契约 | `vkCreatePipelineLayout` | 创建 PSO 时传入 |
| **Pipeline Cache** | 编译结果缓存（**可落盘**） | 跨帧/跨进程复用 | `vkCreatePipelineCache` / `vkGetPipelineCacheData` | `LoadPipelineCache` / `SavePipelineCache`（`:371` / `:1199`） |
| **PSO 预编译（后台）** | 加载期提前编译 | 消除运行时首用卡顿 | 无专门 API（自己开线程调 `vkCreate*Pipelines`） | `PSOPrecompileManager` + `PrecompileQueuePSO` / `StartPSOPrecompile`（**当前禁用**） |
| **GPL（Graphics Pipeline Library）** | **分段**编译/复用 | 顶点输入/预光栅/片元/片元输出四段 | `VK_EXT_graphics_pipeline_library` | `PipelineLibraryCache`；`caps.supportsGraphicsPipelineLibrary` |
| **Dynamic State** | 录制时可改状态 | 减少 PSO 变体 | `vkCmdSetViewport` / `vkCmdSetScissor` 等 | RHI 内封装 |
| **Specialization Constant** | 编译期特化 | 同一 SPIR-V 出多变体 | `VkSpecializationInfo` | — |

## 6. 命令 / 渲染 / 同步

| 名词 | 含义 | 作用 | 原生 API | 本引擎 RHI |
|---|---|---|---|---|
| **Command Pool** | 分配命令缓冲的池 | 与队列族绑定 | `vkCreateCommandPool` / `vkResetCommandPool` | `CreateCommandPools` |
| **Command Buffer** | 录制的命令序列 | primary / **secondary** | `vkAllocateCommandBuffers` / `vkBeginCommandBuffer` / `vkEndCommandBuffer` | `device->CreateSecondaryCommandList()`、临时 Compute 列表 |
| **Secondary CB 调用** | primary 里执行 secondary | 并行录制的载体 | `vkCmdExecuteCommands` | `cmd->ExecuteSecondary(...)` |
| **Render Pass** | 附件与依赖的**模板** | 声明 attachments/subpass/load-store/layout | `vkCreateRenderPass` / `vkCmdBeginRenderPass` | RHI 内封装 |
| **Framebuffer** | 模板的**具体实例** | 把具体 image views 绑到附件槽 | `vkCreateFramebuffer` | RHI 内封装 |
| **Dynamic Rendering** | 免 RenderPass/Framebuffer | 现代替代（1.3 core） | `vkCmdBeginRendering` | RHI 内封装 |
| **Attachment / LoadOp / StoreOp** | 颜色/深度/模板/输入附件与读写策略 | 决定带宽开销 | `VkAttachmentDescription` | 帧图声明资源访问（`ResourceAccess::Read/Write`） |
| **Subpass** | render pass 内的阶段 | tile 内读写（TBDR 关键） | `VkSubpassDescription` | — |
| **Fence** | **CPU↔GPU** 同步 | CPU 等 GPU | `vkCreateFence` / `vkWaitForFences` / `vkResetFences` | `device->WaitForFence(handle, value, timeout)` |
| **Semaphore** | **GPU↔GPU** 同步 | 队列间/acquire-present | `vkCreateSemaphore` | RHI 内封装 |
| **Timeline Semaphore** | 带计数值的信号量 | 跨队列细粒度 signal/wait | `vkSignalSemaphoreKHR` / `vkWaitSemaphoresKHR` | `SignalFenceOnQueue` / `WaitFenceOnQueue`（"空提交"技巧） |
| **Event** | 队列内细粒度同步 | 命令缓冲内 set/wait | `vkCreateEvent` / `vkCmdSetEvent` | — |
| **Barrier** | 依赖 + 布局转换 | execution/memory dependency、layout、队列所有权 | `vkCmdPipelineBarrier` / `vkCmdPipelineBarrier2` | `cmd->PipelineBarrier(...)` |
| **Query Pool** | 查询结果池 | occlusion/timestamp/statistics | `vkCreateQueryPool` / `vkCmdWriteTimestamp` / `vkGetQueryPoolResults` | `ProfilerManager`（注意阻塞式读回会 WAIT） |
| **Indirect Draw** | 绘制参数来自缓冲 | GPU 剔除的出口 | `vkCmdDrawIndexedIndirect` / `…IndirectCount` | `cmd->DrawIndexedIndirect(buf, 0, count, stride)` |
| **DGC** | GPU 生成命令+状态 | 一次执行整段序列 | `vkCmdExecuteGeneratedCommandsEXT`（+ `vkCmdPreprocessGeneratedCommandsEXT`） | `device->InitializeDGC(...)` / `cmd->ExecuteGeneratedCommands(dgcDesc)` / `VulkanDGC` |
| **帧图（RenderGraph）** | 声明式 pass 编排 | 自动依赖/屏障/别名 | 无原生对应 | `rg.AddPass(...)` / `rg.CreateTexture(...)` / `rg.ImportTexture(...)` |
| **多帧在飞行** | N 帧并行 | 隐藏延迟 | 无专门 API | `kMaxFramesInFlight = 3`、`m_Snapshots[MAX_FRAMES_IN_FLIGHT]` |

## 7. 光追专用

| 名词 | 含义 | 作用 | 原生 API | 本引擎 RHI |
|---|---|---|---|---|
| **Acceleration Structure** | BLAS（几何）/ TLAS（实例） | 光追加速结构 | `vkCreateAccelerationStructureKHR` / `vkCmdBuildAccelerationStructuresKHR` | `RTPass` / `VulkanRT`（本会话未逐行读） |
| **Scratch Buffer** | 构建/更新临时内存 | 按 query 结果分配 | `vkGetAccelerationStructureBuildSizesKHR` | 同上 |
| **SBT** | 光追的绑定表 | rgen/rchit/rmiss 索引 | `VkStridedDeviceAddressRegionKHR` | `caps.shaderGroupHandleSize` / `shaderGroupBaseAlignment` |
| **Trace Rays** | 光追派发 | 执行 | `vkCmdTraceRaysKHR` | `RTPass`（含 `maxRTDispatchSize` 等 caps） |
| **Ray Query** | 任意 shader 内查询光线 | 无需 RT 管线 | `VK_KHR_ray_query` | 部分 GI 使用（未逐行读） |
| **Device Address** | 缓冲/AS 的 GPU 地址 | 传裸地址给 shader | `vkGetBufferDeviceAddress` | `sequenceBuffer->GetDeviceAddress()`、`preprocessBufferAddr` |

## 8. 最容易混的 8 对

1. **RenderPass vs Framebuffer** —— 模板 vs 具体绑定
2. **Image vs ImageView vs Sampler** —— 数据 / 视图 / 采样状态
3. **Fence vs Semaphore vs Event** —— CPU↔GPU / GPU↔GPU / 队列内
4. **Command Pool vs Command Buffer vs Secondary CB**
5. **Descriptor Set Layout vs Descriptor Set vs Pipeline Layout**
6. **Pipeline Cache vs Pipeline Library（GPL）** —— 编译缓存 vs 分段复用
7. **Layout vs Format vs Tiling** —— 使用状态 vs 像素格式 vs 内存排布
8. **Acquire vs Present** —— 取图 vs 交回

## 9. 与 D3D 的速查对照

| Vulkan | D3D12 | D3D11 |
|---|---|---|
| RenderPass + Framebuffer | Render Targets（或 `BeginRenderPass`） | `OMSetRenderTargets` |
| Descriptor Set / Heap | Descriptor Heap + Root Signature | 固定槽位 `t/s/u/b` |
| Pipeline | PSO | 状态对象组合（部分） |
| Timeline Semaphore | `ID3D12Fence` | 无 |
| `ExecuteIndirect` / DGC | `ExecuteIndirect`（+ 状态对象） | 仅 `Draw*Indirect` |
| Dynamic Rendering（1.3 core） | Render Passes（可选） | — |
| Bindless（descriptor indexing） | ✅ descriptor heap + SM 6.6 堆索引 | ❌ 不支持（详见 26 号文档 §9） |

## 10. 未复核清单

1. 本文"引擎列"只覆盖**本会话实测**到的调用；RHI 内部封装（`VulkanBuffer` / `VulkanTexture` / `VulkanSwapchain` /
   描述符集实现等）**未逐行读**；
2. 光追部分（Acceleration Structure / SBT / Trace Rays）只依据 `RTPass` 与 `DeviceCaps` 的字段名，**未读实现**；
3. **原生 API 列**是标准 API 常识，未逐条核对规范版本差异（例如 `vkCmdPipelineBarrier2` 需 `synchronization2`）。

---

## 相关文档

- [Vulkan_D3D11_D3D12_Metal主要名称对应.md](Vulkan_D3D11_D3D12_Metal主要名称对应.md)
- [Vulkan渲染一个物体的典型步骤.md](Vulkan渲染一个物体的典型步骤.md)
- [../HugEngine引擎介绍/14.RHI架构与Vulkan实现分析.md](../HugEngine引擎介绍/14.RHI架构与Vulkan实现分析.md)
- [../HugEngine引擎介绍/26.Bindless体系：原理·实现与代码.md](../HugEngine引擎介绍/26.Bindless体系：原理·实现与代码.md)
- [../HugEngine引擎介绍/27.DGC（Device Generated Commands）：原理·实现与代码.md](../HugEngine引擎介绍/27.DGC（Device Generated Commands）：原理·实现与代码.md)
