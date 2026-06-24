# RHI (L2)

渲染硬件接口层 — 多后端抽象，统一 Vulkan/D3D12 接口。

| 子系统 | 说明 | Phase |
|--------|------|-------|
| Public | `IRHIDevice`, `IRHIBuffer`, `IRHITexture`, `IRHICommandList` 等接口定义 | P1 |
| Vulkan | Vulkan 1.3+ 后端实现 (vk::Device, VMA, SPIR-V) | P1 |
| D3D12 | D3D12 SM 6.6+ 后端实现 (ID3D12Device, D3D12MA, DXIL) | P1 |

**依赖**: Core
**关键接口**: `IRHIDevice::CreatePSO()`, `IRHISwapChain::Present()`
