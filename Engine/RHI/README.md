# RHI (L2)

渲染硬件接口层 — 多后端抽象，统一 Vulkan/D3D12 接口。

**设计原则**: 只有 RHI 模块可以直接调用 Vulkan / D3D12 等图形 API。
引擎其他所有模块（Core、Scene、Render、Editor 等）以及 Sample 必须通过 IRHI
公共接口间接使用 GPU 功能，不得直接依赖任何图形 API 头文件。

| 子系统 | 说明 | Phase |
|--------|------|-------|
| Public | `IRHIDevice`, `IRHIBuffer`, `IRHITexture`, `IRHICommandList` 等接口定义 | P1 |
| Vulkan | Vulkan 1.3+ 后端实现 | P1 |

## Vulkan 后端文件结构

| 文件 | 行数 | 职责 |
|------|------|------|
| `VulkanDevice.cpp` | ~600 | 设备生命周期、DescriptorSet 管理、资源创建委托、VulkanDeviceAccess 桥接 |
| `VulkanSwapChain.cpp` | ~200 | 交换链：创建/销毁、窗口缩放、图像获取、呈现 |
| `VulkanCommandList.cpp` | ~750 | 命令录制：Begin/End、RenderPass（含离屏MRT）、Draw/Dispatch、Barrier、Submit |
| `VulkanResources.cpp` | ~630 | 资源实现：Buffer（持久映射）、Texture（含mipmap生成）、Sampler、格式转换 |
| `VulkanPipeline.cpp` | ~320 | PSO 创建：Graphics/Compute 管线构建、RenderPass、PipelineLayout |
| `VulkanInternal.h` | ~320 | 内部头文件：所有 Vulkan 类完整定义 + 共享辅助函数声明 |

**依赖**: Core
**关键接口**: `IRHIDevice::CreatePSO()`, `IRHISwapChain::Present()`
