# Vulkan 渲染一个物体的典型步骤

Vulkan 极其冗长，画一个三角形可能需要 1000+ 行初始化代码。下面按**一次性初始化**和**每帧渲染**两个阶段来梳理。

---

## 一、一次性初始化（启动时）

### 1. 创建实例 — `vkCreateInstance`
- 填写 `VkInstanceCreateInfo`，指定应用名、版本、需要的层（validation layers）和扩展（surface 扩展等）

### 2. 选取物理设备 — `vkEnumeratePhysicalDevices`
- 枚举 GPU，按类型（独显 > 集显）、显存大小、所需队列族等条件评分选出一个 `VkPhysicalDevice`

### 3. 创建逻辑设备 + 队列 — `vkCreateDevice`
- 指定需要的队列族（图形、呈现、传输等）和扩展（如 swapchain）
- 从每个队列族取出 `VkQueue`

### 4. 创建 Surface + Swapchain — `vkCreateSwapchainKHR`
- 创建窗口 Surface → 查询支持的格式/呈现模式 → 创建 Swapchain
- 调用 `vkGetSwapchainImagesKHR` 获取可渲染的图像数组

### 5. 创建 RenderPass — `vkCreateRenderPass`
- 描述：颜色附件格式、加载/存储操作、深度附件等
- 每个 subpass 的依赖关系（用于 subpass 间隐式屏障）

### 6. 创建 Framebuffer — `vkCreateFramebuffer`
- 每个 swapchain image 对应一个 framebuffer，绑定对应的 ImageView（颜色 + 深度）

### 7. 创建描述符集布局 + 管线布局
- **DescriptorSetLayout**: 声明 shader 需要哪些资源（UBO、纹理、采样器），每个绑定的 binding 号
- **PipelineLayout**: 把多个 DescriptorSetLayout + push constant 范围组合起来

### 8. 加载 Shader + 创建 Pipeline — `vkCreateGraphicsPipelines`
- `vkCreateShaderModule` 载入 SPIR-V 字节码
- 填写 `VkGraphicsPipelineCreateInfo`，把所有状态一次性钉死：
  - 着色器阶段（VS + PS）
  - 顶点输入格式 (`VkPipelineVertexInputStateCreateInfo`)
  - 输入装配 (`VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST`)
  - 视口/裁剪 (`VkPipelineViewportStateCreateInfo`)
  - 光栅化 (`VkPipelineRasterizationStateCreateInfo`)
  - 多重采样 (`VkPipelineMultisampleStateCreateInfo`)
  - 深度模板 (`VkPipelineDepthStencilStateCreateInfo`)
  - 颜色混合 (`VkPipelineColorBlendStateCreateInfo`)
  - 动态状态（哪些状态可以不 bake 进 PSO，运行时动态设置）
- **"一锤子买卖"** — PSO 创建后这些状态便固定，要换就得再建一个

### 9. 创建顶点/索引缓冲区 + 分配内存
- `vkCreateBuffer` → `vkGetBufferMemoryRequirements` → `vkAllocateMemory` → `vkBindBufferMemory`
- 将顶点数据通过 staging buffer（CPU 可见）拷贝到 GPU local 内存（`vkCmdCopyBuffer`）

### 10. 创建 Uniform Buffer + 描述符池 + 描述符集
- 创建 UBO buffer（用于 MVP 矩阵等）
- `vkCreateDescriptorPool` 指定各类描述符的最大数
- `vkAllocateDescriptorSets` 分配描述符集
- `vkUpdateDescriptorSets` 将 buffer/texture 绑定到描述符集的具体 binding 上

### 11. 创建同步原语
- `vkCreateSemaphore` — `imageAvailableSemaphore` / `renderFinishedSemaphore`（GPU-GPU）
- `vkCreateFence` — `inFlightFence`（GPU-CPU）

### 12. 创建命令池 + 命令缓冲区
- `vkCreateCommandPool`（每个线程一个）
- `vkAllocateCommandBuffers`（每个 swapchain image 一个，或每帧重新录制）

---

## 二、每帧渲染循环

### Step 1: 等待上一帧完成
```c
vkWaitForFences(device, 1, &inFlightFence, VK_TRUE, UINT64_MAX);
vkResetFences(device, 1, &inFlightFence);
```

### Step 2: 从 Swapchain 获取图像
```c
vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, imageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex);
```

### Step 3: 更新 Uniform Buffer（MVP 矩阵等）
- `memcpy` 到 mapped UBO 内存

### Step 4: 录制命令缓冲区
```c
vkResetCommandBuffer(cmdBuffer, 0);
vkBeginCommandBuffer(cmdBuffer, &beginInfo);
```

然后按顺序录制（每个都是 `vkCmd*` 调用）：

| 顺序 | 操作 | 典型 API |
|------|------|----------|
| 4a | **开始 RenderPass** | `vkCmdBeginRenderPass` |
| 4b | **绑定管线** | `vkCmdBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline)` |
| 4c | **绑定描述符集** | `vkCmdBindDescriptorSets` — 绑定 UBO/纹理等资源 |
| 4d | **绑定顶点缓冲区** | `vkCmdBindVertexBuffers` |
| 4e | **绑定索引缓冲区** (可选) | `vkCmdBindIndexBuffer` |
| 4f | **设置视口 + 裁剪** (如为动态状态) | `vkCmdSetViewport` / `vkCmdSetScissor` |
| 4g | **绘制！** | `vkCmdDraw` / `vkCmdDrawIndexed` |
| 4h | **结束 RenderPass** | `vkCmdEndRenderPass` |
| 4i | **结束录制** | `vkEndCommandBuffer` |

### Step 5: 提交到队列
```c
VkSubmitInfo submitInfo = {
    .waitSemaphoreCount   = 1,
    .pWaitSemaphores      = &imageAvailableSemaphore,   // 等 swapchain image 就绪
    .pWaitDstStageMask    = &waitStages,                // 在颜色输出阶段等待
    .commandBufferCount   = 1,
    .pCommandBuffers      = &cmdBuffer,
    .signalSemaphoreCount = 1,
    .pSignalSemaphores    = &renderFinishedSemaphore,   // 渲染完成后发信号
};
vkQueueSubmit(graphicsQueue, 1, &submitInfo, inFlightFence);
```

### Step 6: 呈现
```c
VkPresentInfoKHR presentInfo = {
    .waitSemaphoreCount = 1,
    .pWaitSemaphores    = &renderFinishedSemaphore,    // 等渲染完成
    .swapchainCount     = 1,
    .pSwapchains        = &swapchain,
    .pImageIndices      = &imageIndex,
};
vkQueuePresentKHR(presentQueue, &presentInfo);
```

---

## 三、流程总览图

```
[初始化阶段 — 仅一次]
Instance → Device → Swapchain → RenderPass → DescriptorSetLayout → PipelineLayout → Pipeline → Buffers+Memory → DescriptorPool/Set → Semaphores+Fences → CommandPool/Buffers

[每帧循环]
  WaitFence ─→ AcquireImage ─→ UpdateUBO ─→ BeginCmdBuffer
      ↑                                          │
      │                                  BeginRenderPass
      │                                          │
      │                                  BindPipeline
      │                                          │
      │                                  BindDescriptorSets
      │                                          │
      │                                  BindVertexBuffer
      │                                          │
      │                                  vkCmdDraw()
      │                                          │
      │                                  EndRenderPass
      │                                          │
      │                                  EndCmdBuffer
      │                                          │
      └──────── Fence ←── QueueSubmit ←─────────┘
                              │
                        signalSemaphore
                              │
                         QueuePresent
```

## 四、与 D3D12 / Metal 的关键差异

| 步骤 | Vulkan | D3D12 | Metal |
|------|--------|-------|-------|
| 管线创建 | `vkCreateGraphicsPipelines` — 一次创建所有状态 | `CreateGraphicsPipelineState` — 类似 PSO | `MTLRenderPipelineDescriptor` → `newRenderPipelineState` |
| RenderPass | 显式 `VkRenderPass` + `VkFramebuffer` 对象 | 无独立对象，在 CommandList 中指定 | `MTLRenderPassDescriptor`（轻量，每帧临时创建） |
| 描述符绑定 | DescriptorSetLayout + Pool + Set 三步 | RootSignature + DescriptorHeap | `[[buffer(N)]]` 属性自动绑定 |
| 同步 | 三件套：Semaphore + Fence + Barrier | Fence + Barrier | 编码器边界自动同步 + Fence/Event 可选 |
| 内存 | 必须手动分配+绑定 | 必须手动管理 | `MTLBuffer` / `MTLTexture` 自动分配，`MTLHeap` 可选 |
| 顶点输入 | `VkPipelineVertexInputStateCreateInfo` | `D3D12_INPUT_LAYOUT_DESC` | `MTLVertexDescriptor` |

> **核心要点：** Vulkan 把一切控制权交给应用——内存、同步、管线状态全都显式声明。这让它极度冗长，但也提供了最精细的性能调优空间。画一个三角形需要 ~1000 行，但换来的是一旦搭好框架，后续渲染循环极其高效且可预测。
