# RHI (L2)

渲染硬件接口层 — 多后端抽象，统一 Vulkan/D3D12 接口。

**设计原则**: 只有 RHI 模块可以直接调用 Vulkan / D3D12 等图形 API。
引擎其他所有模块（Core、Scene、Render、Editor 等）以及 Sample 必须通过 IRHI
公共接口间接使用 GPU 功能，不得直接依赖任何图形 API 头文件。

| 子系统 | 说明 | Phase |
|--------|------|-------|
| Public | `IRHIDevice`, `IRHIBuffer`, `IRHITexture`, `IRHICommandList` 等接口定义 | P1 |
| Vulkan | Vulkan 1.3+ 后端实现 | P1 |

**依赖**: Core
**关键接口**: `IRHIDevice::CreatePSO()`, `IRHISwapChain::Present()`
