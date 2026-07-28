# Vulkan ↔ D3D11 ↔ D3D12 ↔ Metal 主要名称对应

## 核心对象

| 概念 | Vulkan | D3D11 | D3D12 | Metal |
|------|--------|-------|-------|-------|
| 物理设备 | `VkPhysicalDevice` | (无直接对应) | (无直接对应，通过 DXGI Adapter) | (无独立对象，`MTLCreateSystemDefaultDevice()` 返回物理+逻辑合一) |
| 逻辑设备 | `VkDevice` | `ID3D11Device` | `ID3D12Device` | `id<MTLDevice>` |
| 设备上下文 | (内置命令缓冲) | `ID3D11DeviceContext` | (命令列表) | (无独立对象，CommandBuffer 自带上下文) |
| 实例 | `VkInstance` | (无) | (无) | (无) |
| 队列 | `VkQueue` | (隐式) | `ID3D12CommandQueue` | `id<MTLCommandQueue>` |

## 命令记录与提交

| 概念 | Vulkan | D3D11 | D3D12 | Metal |
|------|--------|-------|-------|-------|
| 命令缓冲区 | `VkCommandBuffer` | (Deferred Context) | `ID3D12CommandList` / `ID3D12GraphicsCommandList` | `id<MTLCommandBuffer>` |
| 命令编码器 | (Record 在 CommandBuffer 内) | (隐式) | (无独立概念，直接在 List 中记录) | `id<MTLRenderCommandEncoder>` / `id<MTLBlitCommandEncoder>` / `id<MTLComputeCommandEncoder>` |
| 命令池 | `VkCommandPool` | (无) | `ID3D12CommandAllocator` | (无，CommandBuffer 从 CommandQueue 直接创建) |
| 提交命令 | `vkQueueSubmit` | (自动 / `ExecuteCommandList`) | `ExecuteCommandLists` | `[MTLCommandBuffer commit]` / `enqueue` |
| Fence（栅栏） | `VkFence` | `ID3D11Fence`(11.4+) | `ID3D12Fence` | `id<MTLFence>` / `MTLSharedEvent` (跨进程) |
| 信号量 | `VkSemaphore` | (无直接对应) | (无直接对应，Fence 替代) | `dispatch_semaphore_t` / `id<MTLEvent>` |
| 事件 | `VkEvent` | `ID3D11Query`(部分) | (无直接对应) | `id<MTLEvent>` |
| 屏障/转换 | `VkPipelineBarrier` / `VkImageMemoryBarrier` | (自动) | `ID3D12ResourceBarrier` / `ResourceBarrier` | `MTLFence` + `MTLBarrier` (blit/compute encoder 内) / `MTLRenderStages` |

## 管线与着色器

| 概念 | Vulkan | D3D11 | D3D12 | Metal |
|------|--------|-------|-------|-------|
| 着色器库 | (无独立概念) | (无) | (无) | `id<MTLLibrary>` (从源码或预编译 metal 二进制加载) |
| 着色器函数/入口 | (入口名在 `VkPipelineShaderStageCreateInfo` 中指定) | (字节码中内嵌入口) | (字节码中内嵌入口) | `id<MTLFunction>` (从 `MTLLibrary` 按名获取) |
| 着色器模块 | `VkShaderModule` | 编译后字节码直接传入 | `D3D12_SHADER_BYTECODE` | (见 `MTLLibrary`，无独立模块对象) |
| 管线布局 | `VkPipelineLayout` | (自动绑定) | `ID3D12RootSignature` | (自动，通过 Shader 中的 `[[buffer(N)]]` / `[[texture(N)]]` 属性绑定) |
| 参数编码器 | (描述符集布局 + 更新) | (自动) | (描述符堆) | `id<MTLArgumentEncoder>` (Metal 2，类似描述符集) |
| 图形管线 | `VkPipeline` | `ID3D11VertexShader` + `ID3D11PixelShader` 等分散对象 | `ID3D12PipelineState` | `id<MTLRenderPipelineState>` |
| 计算管线 | `VkPipeline` (compute) | `ID3D11ComputeShader` | `ID3D12PipelineState` (compute) | `id<MTLComputePipelineState>` |
| 网格/对象管线 | `VkPipeline` (mesh) | (不支持) | `ID3D12PipelineState` (mesh) | `id<MTLMeshRenderPipelineState>` (Metal 3, 间接绘制) |
| 描述符集布局 | `VkDescriptorSetLayout` | (自动) | Root Signature 部分 | `MTLArgumentEncoder` |
| 描述符集 | `VkDescriptorSet` | SRV/UAV/CBV 绑定自动管理 | 描述符堆 + 描述符表 | `MTLArgumentEncoder` 写入 `MTLBuffer` + `useResource` 绑定 |
| 顶点输入状态 | `VkPipelineVertexInputStateCreateInfo` | `ID3D11InputLayout` | `D3D12_INPUT_LAYOUT_DESC` | `MTLVertexDescriptor` (在 `MTLRenderPipelineDescriptor` 中) |
| 光栅化状态 | `VkPipelineRasterizationStateCreateInfo` | `ID3D11RasterizerState` | `D3D12_RASTERIZER_DESC` | (内联在 `MTLRenderPipelineDescriptor` 中，无独立对象) |
| 深度模板状态 | `VkPipelineDepthStencilStateCreateInfo` | `ID3D11DepthStencilState` | `D3D12_DEPTH_STENCIL_DESC` | `id<MTLDepthStencilState>` |
| 混合状态 | `VkPipelineColorBlendStateCreateInfo` | `ID3D11BlendState` | `D3D12_BLEND_DESC` | (内联在 `MTLRenderPipelineColorAttachmentDescriptor` 中) |

## 资源

| 概念 | Vulkan | D3D11 | D3D12 | Metal |
|------|--------|-------|-------|-------|
| 缓冲区 | `VkBuffer` | `ID3D11Buffer` | `ID3D12Resource`（含 buffer） | `id<MTLBuffer>` |
| 图像/纹理 | `VkImage` | `ID3D11Texture1D/2D/3D` | `ID3D12Resource`（含 texture） | `id<MTLTexture>` |
| 设备内存 / 堆 | `VkDeviceMemory` | (隐式) | `ID3D12Heap` | `id<MTLHeap>` (Metal 2，显式内存管理) |
| 缓冲区视图 | `VkBufferView` | SRV | 描述符 | (无独立对象，通过 offset + length 绑定) |
| 图像视图 | `VkImageView` | SRV / RTV / DSV | 描述符 | `[MTLTexture newTextureViewWithPixelFormat:...]` (创建新纹理视图) |
| 采样器 | `VkSampler` | `ID3D11SamplerState` | 描述符（静态采样器或堆内） | `id<MTLSamplerState>` |

## 描述符 / 资源绑定

| 概念 | Vulkan | D3D11 | D3D12 | Metal |
|------|--------|-------|-------|-------|
| 常量缓冲区 | Uniform Buffer | Constant Buffer (`ID3D11Buffer`) | CBV (Constant Buffer View) | `MTLBuffer` (在 `constant` 地址空间，通过 `[[buffer(N)]]` 绑定) |
| 着色器资源（纹理） | Sampled Image | Shader Resource View (SRV) | SRV | `MTLTexture` (在 `texture` 地址空间，通过 `[[texture(N)]]` 绑定) |
| 无序访问（读写纹理） | Storage Image | Unordered Access View (UAV) | UAV | `MTLTexture` (在 `texture` 地址空间，标记 `access::read_write`) |
| 无序访问（读写缓冲） | Storage Buffer | UAV (buffer) | UAV (buffer) | `MTLBuffer` (在 `device` 地址空间，标记 `access::read_write`) |
| 采样器 | Sampler (描述符) | Sampler State | Sampler 描述符 | `MTLSamplerState` (通过 `[[sampler(N)]]` 绑定) |

## 渲染目标与输出

| 概念 | Vulkan | D3D11 | D3D12 | Metal |
|------|--------|-------|-------|-------|
| 渲染通道 | `VkRenderPass` | (隐式) | (无独立对象，在命令中指定) | `MTLRenderPassDescriptor` |
| 帧缓冲 | `VkFramebuffer` | Render Target Views 绑定 | RTV + DSV 描述符句柄 | (无独立对象，在 `MTLRenderPassDescriptor` 的附件中设置) |
| 视口 | `VkViewport` | `D3D11_VIEWPORT` | `D3D12_VIEWPORT` | `MTLViewport` |
| 裁剪矩形 | `VkRect2D` | `D3D11_RECT` | `D3D12_RECT` | `MTLScissorRect` |
| 颜色附件 | `VkAttachmentDescription` | Render Target View (RTV) | RTV 描述符 | `MTLRenderPassColorAttachmentDescriptor` |
| 深度附件 | (见深度模板附件) | Depth Stencil View (DSV) | DSV 描述符 | `MTLRenderPassDepthAttachmentDescriptor` |
| 模板附件 | (见深度模板附件) | Depth Stencil View (DSV) | DSV 描述符 | `MTLRenderPassStencilAttachmentDescriptor` |

## 同步与内存

| 概念 | Vulkan | D3D11 | D3D12 | Metal |
|------|--------|-------|-------|-------|
| 资源状态跟踪 | 显式 Image Layout 转换 | 自动（驱动管理） | 显式 Resource States | 自动（Metal 驱动管理资源依赖，App 无需显式 Barrier） |
| 编码器内屏障 | Pipeline Barrier 系列 | (自动) | Resource Barrier 系列 | `MTLFence` + `[MTLRenderCommandEncoder waitForFence:...]` / `updateFence:...` |
| 编码器间屏障 | Pipeline Barrier 系列 | (自动) | (通过 CommandList 提交顺序 + Barrier) | (编码器顺序自然保证，或使用 `MTLFence` + `MTLEvent`) |
| GPU-CPU 同步 | Fence + `vkWaitForFences` | Fence / Query | Fence + `SetEventOnCompletion` | `[MTLCommandBuffer waitUntilCompleted]` / `addCompletedHandler:` |
| 子资源 | `VkImageSubresourceRange` | `D3D11CalcSubresource` | `D3D12CalcSubresource` / 子资源索引 | `[MTLTexture newTextureViewWithPixelFormat:textureType:levels:slices:]` |
| 内存映射 | `vkMapMemory` | `Map` / `Unmap` | `Map` / `Unmap` + `ID3D12Resource::Map` | `[MTLBuffer contents]` (返回直接可写指针，无需 Map/Unmap) |
| 上传资源 | Staging Buffer + `vkCmdCopyBuffer` | `Map` + `UpdateSubresource` | Upload Heap + Copy | `MTLBuffer` (Shared 模式) + `[MTLBlitCommandEncoder copyFromBuffer:...]` |

## 查询 / 性能计数

| 概念 | Vulkan | D3D11 | D3D12 | Metal |
|------|--------|-------|-------|-------|
| 查询池 | `VkQueryPool` | `ID3D11Query` | `ID3D12QueryHeap` | `id<MTLCounterSampleBuffer>` (Metal GPU Family 3+) |
| 遮挡查询 | Occlusion Query | `D3D11_QUERY_OCCLUSION` | `D3D12_QUERY_TYPE_OCCLUSION` | `[MTLRenderCommandEncoder setVisibilityResultMode:...]` (写入 buffer 指定 offset) |
| 时间戳查询 | Timestamp Query | `D3D11_QUERY_TIMESTAMP` | `D3D12_QUERY_TYPE_TIMESTAMP` | `MTLCounterSampleBuffer` + 时间戳 counter set |
| 管线统计 | Pipeline Statistics Query | `D3D11_QUERY_PIPELINE_STATISTICS` | `D3D12_QUERY_TYPE_PIPELINE_STATISTICS` | `MTLCounterSampleBuffer` + 统计 counter set |

## 着色器语言与编译

| 概念 | Vulkan | D3D11 | D3D12 | Metal |
|------|--------|-------|-------|-------|
| 着色器语言 | GLSL / HLSL (通过 SPIR-V) | HLSL | HLSL | Metal Shading Language (MSL，基于 C++14) |
| 中间表示 | SPIR-V | DXBC / DXIL | DXBC / DXIL | Metal IR (Air) / MetalLib (预编译二进制) |
| 运行时编译 | `vkCreateShaderModule` (SPIR-V 输入) | `D3DCompile` / `D3DCompileFromFile` | `D3DCompile` / `D3DCompileFromFile` | `[MTLDevice newLibraryWithSource:...]` (运行时编译 MSL 源码) |
| 预编译 | 离线 SPIR-V 编译 | 离线 HLSL 编译 | 离线 HLSL 编译 | 离线 metal 工具链 (`xcrun metal` → `.metallib`) |
| 编译优化 | (由 SPIR-V 前端和驱动完成) | 编译器 + 驱动 | 编译器 + 驱动 | Xcode Metal 编译器 + 驱动 |

## 着色器阶段

| 概念 | Vulkan | D3D11 | D3D12 | Metal |
|------|--------|-------|-------|-------|
| 顶点着色器 | `VK_SHADER_STAGE_VERTEX_BIT` | Vertex Shader (VS) | VS | `vertex` 函数 |
| 像素/片元着色器 | `VK_SHADER_STAGE_FRAGMENT_BIT` | Pixel Shader (PS) | PS | `fragment` 函数 |
| 几何着色器 | `VK_SHADER_STAGE_GEOMETRY_BIT` | Geometry Shader (GS) | GS | (不支持) |
| 计算着色器 | `VK_SHADER_STAGE_COMPUTE_BIT` | Compute Shader (CS) | CS | `kernel` 函数 |
| 细分控制 / 外壳 | `VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT` | Hull Shader (HS) | HS | (无独立阶段，通过 `MTLTessellationFactorStepFunction` 控制) |
| 细分评估 / 域 | `VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT` | Domain Shader (DS) | DS | post-tessellation `vertex` 函数 (读取 `[[instance_id]]`) |
| 对象着色器 | `VK_SHADER_STAGE_TASK_BIT_EXT` | (不支持) | Amplification Shader (AS) | `object` 函数 (Metal 3, Apple Silicon) |
| 网格着色器 | `VK_SHADER_STAGE_MESH_BIT_EXT` | (不支持) | Mesh Shader (MS) | `mesh` 函数 (Metal 3, Apple Silicon) |
| 光线追踪 - 生成 | `VK_SHADER_STAGE_RAYGEN_BIT_KHR` | (不支持) | Ray Generation | `raygen` 限定符 (Metal 3, Apple Silicon) |
| 光线追踪 - 相交 | `VK_SHADER_STAGE_INTERSECTION_BIT_KHR` | (不支持) | Intersection | `intersection` 限定符 (Metal 3) |
| 光线追踪 - 命中/未命中 | `VK_SHADER_STAGE_ANY_HIT_BIT_KHR` / `CLOSEST_HIT_BIT_KHR` / `MISS_BIT_KHR` | (不支持) | Any Hit / Closest Hit / Miss | 对应 `intersection` / `closest_hit` / `miss` 限定符 (Metal 3) |

## 间接绘制与 GPU 驱动

| 概念 | Vulkan | D3D11 | D3D12 | Metal |
|------|--------|-------|-------|-------|
| 间接绘制 | `vkCmdDrawIndirect` | `DrawInstancedIndirect` | `ExecuteIndirect` | `[MTLRenderCommandEncoder drawPrimitives:indirectBuffer:indirectBufferOffset:]` |
| 间接计算调度 | `vkCmdDispatchIndirect` | `DispatchIndirect` | `ExecuteIndirect` (compute) | `[MTLComputeCommandEncoder dispatchThreadgroupsWithIndirectBuffer:...]` |
| 间接命令缓冲 | `VkIndirectCommandsLayoutNV` (VK_NV_device_generated_commands) | (不支持) | `ID3D12CommandSignature` + GPU-driven ExecuteIndirect | `id<MTLIndirectCommandBuffer>` (Metal 2, Apple Silicon) |

## 关键设计差异总结

| 方面 | D3D11 | Vulkan / D3D12 / Metal |
|------|-------|-------------------------|
| 资源状态 | 驱动自动管理 | Vulkan/D3D12 **应用显式管理**；Metal **驱动自动追踪** |
| 内存管理 | 驱动自动分配 | Vulkan/D3D12 **应用显式管理**；Metal **偏向自动** (`MTLHeap` 可选) |
| 描述符/绑定 | 驱动管理、自动优化 | Vulkan **Descriptor Set** / D3D12 **Root Signature + 描述符堆** / Metal **参数绑定属性 ([[buffer(N)]])** |
| 管线状态 | 分散的独立状态对象 | **整体 PSO（Pipeline State Object）** |
| 同步 | 驱动隐式处理 | Vulkan **显式 Barrier/Fence** / D3D12 **显式 Barrier/Fence** / Metal **半自动** (编码器边界自动 + Fence/Event 可选) |
| 多线程 | 有限支持 (Deferred Context) | **一流的多线程支持** |
| 错误处理 | 驱动内部处理 | **更直接的错误反馈** |
| 着色器语言 | HLSL | Vulkan **SPIR-V** / D3D12 **HLSL** / Metal **MSL (C++14)** |
| 运行时编译 | 直接编译 HLSL | Vulkan 需 SPIR-V 预编译 / D3D12 支持 DXBC/DXIL / Metal 支持运行时 MSL 编译 |

## 四大 API 生态对比

| 维度 | Vulkan | D3D11 | D3D12 | Metal |
|------|--------|-------|-------|-------|
| 平台 | Windows / Linux / Android / Switch | Windows | Windows / Xbox | macOS / iOS / iPadOS / tvOS / visionOS |
| 设计哲学 | 显式、低层级、最大控制 | 隐式、高层级、易上手 | 显式、低层级、介于 VK 与 Metal 之间 | 低层级但偏实用、Apple 硬件协同设计 |
| 学习曲线 | 最陡 | 最低 | 陡峭 | 中等 |
| 多线程友好度 | ★★★★★ | ★★☆☆☆ (Deferred Context) | ★★★★★ | ★★★★★ |
| 显式内存管理 | 是 | 否 | 是 | 可选 (`MTLHeap` / Automatic) |
| 显式同步 | 是 | 否 | 是 | 部分 (编码器边界自动) |
| Tile Memory / TBDR | 部分支持 (subpass) | 有限 | 有限 | 一流支持 (memoryless attachments, imageblock) |
| GPU 驱动工作 | 完全无 | 全部由驱动完成 | 极少 | 极少 |

> **一句话总结：** Vulkan 和 D3D12 是"给你钥匙但让你自己开车"的风格，D3D11 是"自动挡代驾"，而 Metal 则是在"给你钥匙"和"帮你搞定"之间找到了 Apple 式的平衡——TBDR 优化和 Swift/ObjC 原生集成是其独有优势，但仅限 Apple 平台。
