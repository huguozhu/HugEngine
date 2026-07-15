# VMA 集成：Vulkan Memory Allocator 替换裸分配

> 日期：2026-07-03 | 状态：设计完成 | Phase：P1

## 1. 目标

用 VMA（Vulkan Memory Allocator）替换当前 RHI Vulkan 后端中的裸 `vkAllocateMemory`/`vkFreeMemory`，
获得内存池化、子分配、碎片整理能力，消除 `maxMemoryAllocationCount` 瓶颈。

## 2. 架构

```
VulkanDevice
  ├── m_VmaAllocator ────────────── VMA 全局分配器（Init 创建，Shutdown 销毁）
  │
  ├── CreateBuffer()  ──► VulkanBuffer(VmaAllocator)
  │     vmaCreateBuffer() 一步完成 create+bind+allocate
  │
  ├── CreateTexture() ──► VulkanTexture(VmaAllocator)
  │     vkCreateImage() + vmaAllocateMemoryForImage() + vmaBindImageMemory()
  │
  └── 纹理上传暂存 buffer 也用 VMA
```

## 3. 文件清单

| 操作 | 文件 | 内容 |
|------|------|------|
| 新增 | `Engine/External/VulkanMemoryAllocator/vk_mem_alloc.h` | VMA 单头文件 |
| 修改 | `Engine/RHI/Vulkan/VulkanInternal.h` | VulkanDevice 增加 `VmaAllocator m_VmaAllocator` |
| 修改 | `Engine/RHI/Vulkan/VulkanDevice.cpp` | Init 创建 VmaAllocator，Shutdown 销毁 |
| 修改 | `Engine/RHI/Vulkan/VulkanResources.cpp` | Buffer/Texture 改用 VMA API |
| 修改 | `Engine/RHI/Vulkan/CMakeLists.txt` | 添加 VMA include 路径 |

## 4. API 映射

| 裸 Vulkan | VMA 替代 |
|-----------|---------|
| `vkCreateBuffer` + `vkAllocateMemory` + `vkBindBufferMemory` | `vmaCreateBuffer` |
| `vkDestroyBuffer` + `vkFreeMemory` | `vmaDestroyBuffer` |
| `vkMapMemory` | `vmaMapMemory` |
| `vkUnmapMemory` | `vmaUnmapMemory` |
| `vkCreateImage` + `vkAllocateMemory` + `vkBindImageMemory` | `vkCreateImage` + `vmaAllocateMemoryForImage` + `vmaBindImageMemory` |
| `vkDestroyImage` + `vkFreeMemory` | `vkDestroyImage` + `vmaFreeMemory` |
| `FindMemoryType()` | 删除（VMA 自动选择） |

## 5. VMA 配置

- `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` — 引擎使用 buffer device address
- Buffer：`VMA_MEMORY_USAGE_AUTO` — VMA 根据 usage 自动选 HOST_VISIBLE 或 DEVICE_LOCAL
- Texture：`VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE` — 优先 GPU 本地内存
- Staging：`VMA_MEMORY_USAGE_CPU_ONLY` — 仅 CPU 写入
- Vulkan 1.3 函数指针通过 `VmaVulkanFunctions` 传入

## 6. 不变接口

- IRHI 公共接口（`IRHIBuffer`、`IRHITexture` 等）完全不变
- `VulkanBuffer` 和 `VulkanTexture` 对外行为一致
- `GetNativeHandle()` / `GetDeviceAddress()` 语义不变

## 7. 测试

- 04.Deferred Sponza 场景正常运行
- Vulkan 验证层无内存泄漏
- 所有 Buffer/Texture/Sampler 正确创建和销毁
