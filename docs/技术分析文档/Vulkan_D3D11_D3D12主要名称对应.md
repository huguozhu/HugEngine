# Vulkan ↔ D3D11 ↔ D3D12 主要名称对应

## 核心对象

| 概念 | Vulkan | D3D11 | D3D12 |
|------|--------|-------|-------|
| 物理设备 | `VkPhysicalDevice` | (无直接对应) | (无直接对应，通过 DXGI Adapter) |
| 逻辑设备 | `VkDevice` | `ID3D11Device` | `ID3D12Device` |
| 设备上下文 | (内置命令缓冲) | `ID3D11DeviceContext` | (命令列表) |
| 实例 | `VkInstance` | (无) | (无) |
| 队列 | `VkQueue` | (隐式) | `ID3D12CommandQueue` |

## 命令记录与提交

| 概念 | Vulkan | D3D11 | D3D12 |
|------|--------|-------|-------|
| 命令缓冲区 | `VkCommandBuffer` | (Deferred Context) | `ID3D12CommandList` / `ID3D12GraphicsCommandList` |
| 命令池 | `VkCommandPool` | (无) | `ID3D12CommandAllocator` |
| 提交命令 | `vkQueueSubmit` | (自动 / `ExecuteCommandList`) | `ExecuteCommandLists` |
| Fence（栅栏） | `VkFence` | `ID3D11Fence`(11.4+) | `ID3D12Fence` |
| 信号量 | `VkSemaphore` | (无直接对应) | (无直接对应，Fence 替代) |
| 事件 | `VkEvent` | `ID3D11Query`(部分) | (无直接对应) |
| 屏障/转换 | `VkPipelineBarrier` / `VkImageMemoryBarrier` | (自动) | `ID3D12ResourceBarrier` / `ResourceBarrier` |

## 管线与着色器

| 概念 | Vulkan | D3D11 | D3D12 |
|------|--------|-------|-------|
| 着色器模块 | `VkShaderModule` | 编译后字节码直接传入 | `D3D12_SHADER_BYTECODE` |
| 管线布局 | `VkPipelineLayout` | (自动绑定) | `ID3D12RootSignature` |
| 图形管线 | `VkPipeline` | `ID3D11VertexShader` + `ID3D11PixelShader` 等分散对象 | `ID3D12PipelineState` |
| 计算管线 | `VkPipeline` (compute) | `ID3D11ComputeShader` | `ID3D12PipelineState` (compute) |
| 描述符集布局 | `VkDescriptorSetLayout` | (自动) | Root Signature 部分 |
| 描述符集 | `VkDescriptorSet` | SRV/UAV/CBV 绑定自动管理 | 描述符堆 + 描述符表 |
| 顶点输入状态 | `VkPipelineVertexInputStateCreateInfo` | `ID3D11InputLayout` | `D3D12_INPUT_LAYOUT_DESC` |
| 光栅化状态 | `VkPipelineRasterizationStateCreateInfo` | `ID3D11RasterizerState` | `D3D12_RASTERIZER_DESC` |
| 深度模板状态 | `VkPipelineDepthStencilStateCreateInfo` | `ID3D11DepthStencilState` | `D3D12_DEPTH_STENCIL_DESC` |
| 混合状态 | `VkPipelineColorBlendStateCreateInfo` | `ID3D11BlendState` | `D3D12_BLEND_DESC` |

## 资源

| 概念 | Vulkan | D3D11 | D3D12 |
|------|--------|-------|-------|
| 缓冲区 | `VkBuffer` | `ID3D11Buffer` | `ID3D12Resource`（含 buffer） |
| 图像/纹理 | `VkImage` | `ID3D11Texture1D/2D/3D` | `ID3D12Resource`（含 texture） |
| 设备内存 | `VkDeviceMemory` | (隐式) | `ID3D12Heap` |
| 缓冲区视图 | `VkBufferView` | SRV | 描述符 |
| 图像视图 | `VkImageView` | SRV / RTV / DSV | 描述符 |
| 采样器 | `VkSampler` | `ID3D11SamplerState` | 描述符（静态采样器或堆内） |

## 描述符 / 资源绑定

| 概念 | Vulkan | D3D11 | D3D12 |
|------|--------|-------|-------|
| 常量缓冲区 | Uniform Buffer | Constant Buffer (`ID3D11Buffer`) | CBV (Constant Buffer View) |
| 着色器资源（纹理） | Sampled Image | Shader Resource View (SRV) | SRV |
| 无序访问 | Storage Image / Buffer | Unordered Access View (UAV) | UAV |
| 采样器 | Sampler (描述符) | Sampler State | Sampler 描述符 |

## 渲染目标与输出

| 概念 | Vulkan | D3D11 | D3D12 |
|------|--------|-------|-------|
| 渲染通道 | `VkRenderPass` | (隐式) | (无独立对象，在命令中指定) |
| 帧缓冲 | `VkFramebuffer` | Render Target Views 绑定 | RTV + DSV 描述符句柄 |
| 视口 | `VkViewport` | `D3D11_VIEWPORT` | `D3D12_VIEWPORT` |
| 裁剪矩形 | `VkRect2D` | `D3D11_RECT` | `D3D12_RECT` |
| 颜色附件 | `VkAttachmentDescription` | Render Target View (RTV) | RTV 描述符 |
| 深度模板附件 | 同上 | Depth Stencil View (DSV) | DSV 描述符 |

## 同步与内存

| 概念 | Vulkan | D3D11 | D3D12 |
|------|--------|-------|-------|
| 资源状态跟踪 | 显式 Image Layout 转换 | 自动（驱动管理） | 显式 Resource States |
| 子资源 | `VkImageSubresourceRange` | `D3D11CalcSubresource` | `D3D12CalcSubresource` / 子资源索引 |
| 内存映射 | `vkMapMemory` | `Map` / `Unmap` | `Map` / `Unmap` + `ID3D12Resource::Map` |
| 上传资源 | Staging Buffer + `vkCmdCopyBuffer` | `Map` + `UpdateSubresource` | Upload Heap + Copy |

## 查询

| 概念 | Vulkan | D3D11 | D3D12 |
|------|--------|-------|-------|
| 查询池 | `VkQueryPool` | `ID3D11Query` | `ID3D12QueryHeap` |
| 遮挡查询 | Occlusion Query | `D3D11_QUERY_OCCLUSION` | `D3D12_QUERY_TYPE_OCCLUSION` |
| 时间戳查询 | Timestamp Query | `D3D11_QUERY_TIMESTAMP` | `D3D12_QUERY_TYPE_TIMESTAMP` |
| 管线统计 | Pipeline Statistics Query | `D3D11_QUERY_PIPELINE_STATISTICS` | `D3D12_QUERY_TYPE_PIPELINE_STATISTICS` |

## 着色器阶段

| 概念 | Vulkan | D3D11 | D3D12 |
|------|--------|-------|-------|
| 顶点着色器 | `VK_SHADER_STAGE_VERTEX_BIT` | Vertex Shader (VS) | VS |
| 像素/片元着色器 | `VK_SHADER_STAGE_FRAGMENT_BIT` | Pixel Shader (PS) | PS |
| 几何着色器 | `VK_SHADER_STAGE_GEOMETRY_BIT` | Geometry Shader (GS) | GS |
| 计算着色器 | `VK_SHADER_STAGE_COMPUTE_BIT` | Compute Shader (CS) | CS |
| 细分控制/外壳 | `VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT` | Hull Shader (HS) | HS |
| 细分评估/域 | `VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT` | Domain Shader (DS) | DS |
| 网格着色器 | `VK_SHADER_STAGE_MESH_BIT_EXT` | (不支持) | Amplification Shader (AS) / Mesh Shader (MS) |
| 光线追踪 | Raygen/Miss/Hit/etc 阶段 | (不支持) | Raygen/Miss/Hit/etc 阶段 |

## 关键设计差异总结

| 方面 | D3D11 | Vulkan / D3D12 |
|------|-------|----------------|
| 资源状态 | 驱动自动管理 | **应用显式管理** |
| 内存管理 | 驱动自动分配 | **应用显式管理** |
| 描述符/绑定 | 驱动管理、自动优化 | **应用显式管理** (Root Signature / Descriptor Set) |
| 管线状态 | 分散的独立状态对象 | **整体 PSO（Pipeline State Object）** |
| 同步 | 驱动隐式处理 | **应用显式 Barrier / Fence** |
| 多线程 | 有限支持 (Deferred Context) | **一流的多线程支持** |
| 错误处理 | 驱动内部处理 | **更直接的错误反馈** |

> Vulkan 和 D3D12 在概念上非常接近（都是低层级显式 API），而 D3D11 则隐藏了大量细节在驱动层。在做跨平台或 API 迁移时，Vulkan ↔ D3D12 的映射通常更为自然。
