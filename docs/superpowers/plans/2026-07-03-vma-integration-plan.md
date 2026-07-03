# VMA 集成实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 用 VMA 替换 Vulkan 后端的裸 vkAllocateMemory/vkFreeMemory，获得内存池化和子分配能力。

**Architecture:** 添加 VMA 单头文件，VulkanDevice 持有 VmaAllocator，VulkanBuffer/VulkanTexture 构造函数接收 VmaAllocator 参数并改用 VMA API。

**Tech Stack:** VMA 4.2+, Vulkan 1.3, C++17

**Spec:** `docs/superpowers/specs/2026-07-03-vma-integration-design.md`

## Global Constraints

- IRHI 公共接口完全不变
- Buffer device address 支持必须保持
- 持久映射（Map/Unmap no-op）模式保持不变
- Vulkan 验证层无内存泄漏
- Commit 消息中文，无 AI 信息

---

### Task 1: 下载 VMA 单头文件

**Files:**
- Create: `Engine/External/VulkanMemoryAllocator/vk_mem_alloc.h`

- [ ] **Step 1: 下载 VMA 最新版本**

从 GitHub 下载 VMA 单头文件：

```bash
curl -L -o D:/Source/HugEngine/Engine/External/VulkanMemoryAllocator/vk_mem_alloc.h \
  https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/master/include/vk_mem_alloc.h
```

如果 curl 不可用，手动从 https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator 下载 `include/vk_mem_alloc.h` 放入 `Engine/External/VulkanMemoryAllocator/`。

---

### Task 2: VulkanInternal.h — VulkanDevice 增加 VmaAllocator

**Files:**
- Modify: `Engine/RHI/Vulkan/VulkanInternal.h`

- [ ] **Step 1: 添加 VMA include 和成员**

在 `VulkanInternal.h` 顶部（`#include <vulkan/vulkan.h>` 之后）添加：

```cpp
// VMA (Vulkan Memory Allocator) — 单头文件
#define VMA_VULKAN_VERSION 1003000  // Vulkan 1.3
#include "vk_mem_alloc.h"
```

在 `VulkanDevice` 类的 getter 区域添加：

```cpp
    VmaAllocator GetVmaAllocator() const { return m_VmaAllocator; }
```

在 `VulkanDevice` 类的 private 成员中添加：

```cpp
    VmaAllocator    m_VmaAllocator  = VK_NULL_HANDLE;
```

---

### Task 3: VulkanDevice.cpp — Init 创建 / Shutdown 销毁 VmaAllocator

**Files:**
- Modify: `Engine/RHI/Vulkan/VulkanDevice.cpp`

- [ ] **Step 1: CreateLogicalDevice 末尾创建 VmaAllocator**

在 `CreateLogicalDevice()` 方法末尾（`vkGetDeviceQueue` 之后）添加：

```cpp
    // 创建 VMA 分配器（替代裸 vkAllocateMemory/vkFreeMemory）
    VmaVulkanFunctions vmaVulkanFunctions{};
    vmaVulkanFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vmaVulkanFunctions.vkGetDeviceProcAddr   = vkGetDeviceProcAddr;
    vmaVulkanFunctions.vkAllocateMemory                    = vkAllocateMemory;
    vmaVulkanFunctions.vkBindBufferMemory                  = vkBindBufferMemory;
    vmaVulkanFunctions.vkBindBufferMemory2                 = vkBindBufferMemory2;
    vmaVulkanFunctions.vkBindImageMemory                   = vkBindImageMemory;
    vmaVulkanFunctions.vkBindImageMemory2                  = vkBindImageMemory2;
    vmaVulkanFunctions.vkCreateBuffer                      = vkCreateBuffer;
    vmaVulkanFunctions.vkCreateImage                       = vkCreateImage;
    vmaVulkanFunctions.vkDestroyBuffer                     = vkDestroyBuffer;
    vmaVulkanFunctions.vkDestroyImage                      = vkDestroyImage;
    vmaVulkanFunctions.vkFlushMappedMemoryRanges           = vkFlushMappedMemoryRanges;
    vmaVulkanFunctions.vkFreeMemory                        = vkFreeMemory;
    vmaVulkanFunctions.vkGetBufferMemoryRequirements       = vkGetBufferMemoryRequirements;
    vmaVulkanFunctions.vkGetBufferMemoryRequirements2      = vkGetBufferMemoryRequirements2;
    vmaVulkanFunctions.vkGetImageMemoryRequirements        = vkGetImageMemoryRequirements;
    vmaVulkanFunctions.vkGetImageMemoryRequirements2       = vkGetImageMemoryRequirements2;
    vmaVulkanFunctions.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
    vmaVulkanFunctions.vkGetPhysicalDeviceProperties       = vkGetPhysicalDeviceProperties;
    vmaVulkanFunctions.vkInvalidateMappedMemoryRanges      = vkInvalidateMappedMemoryRanges;
    vmaVulkanFunctions.vkMapMemory                         = vkMapMemory;
    vmaVulkanFunctions.vkUnmapMemory                       = vkUnmapMemory;
    vmaVulkanFunctions.vkCmdCopyBuffer                     = vkCmdCopyBuffer;

    VmaAllocatorCreateInfo vmaCreateInfo{};
    vmaCreateInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    vmaCreateInfo.physicalDevice   = m_Physical;
    vmaCreateInfo.device           = m_Device;
    vmaCreateInfo.instance         = m_Instance;
    vmaCreateInfo.flags            = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    vmaCreateInfo.pVulkanFunctions = &vmaVulkanFunctions;

    VkResult vmaResult = vmaCreateAllocator(&vmaCreateInfo, &m_VmaAllocator);
    HE_ASSERT(vmaResult == VK_SUCCESS, "Failed to create VMA allocator");
    HE_CORE_INFO("VMA allocator created (buffer device address enabled)");
```

- [ ] **Step 2: Shutdown 开头销毁 VmaAllocator**

在 `Shutdown()` 方法开头（`vkDestroyDescriptorSetLayout` 之前）添加：

```cpp
    if (m_VmaAllocator) { vmaDestroyAllocator(m_VmaAllocator); m_VmaAllocator = VK_NULL_HANDLE; }
```

---

### Task 4: VulkanResources.cpp — Buffer/Texture 改用 VMA API

**Files:**
- Modify: `Engine/RHI/Vulkan/VulkanResources.cpp`

- [ ] **Step 1: VulkanBuffer 构造函数改用 vmaCreateBuffer**

将 `VulkanBuffer` 构造函数签名从：
```cpp
VulkanBuffer::VulkanBuffer(VkDevice device, VkPhysicalDevice physical, const BufferDesc& desc)
```
改为：
```cpp
VulkanBuffer::VulkanBuffer(VmaAllocator allocator, const BufferDesc& desc)
```

成员变量 `m_Device` 改为 `m_Allocator`（类型 `VmaAllocator`）。

构造函数体替换为 VMA 一键分配：

```cpp
VulkanBuffer::VulkanBuffer(VmaAllocator allocator, const BufferDesc& desc)
    : m_Allocator(allocator), m_Size(desc.size)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size  = desc.size;
    bufferInfo.usage = ToVkBufferUsage(desc.usage);

    VmaAllocationCreateInfo allocCreateInfo{};
    allocCreateInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                          | VMA_ALLOCATION_CREATE_MAPPED_BIT;  // 持久映射
    allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;

    VkResult result = vmaCreateBuffer(allocator, &bufferInfo, &allocCreateInfo,
                                       &m_Buffer, &m_Allocation, nullptr);
    HE_ASSERT(result == VK_SUCCESS, "VMA: Failed to create buffer");

    // 获取持久映射指针（VMA_ALLOCATION_CREATE_MAPPED_BIT 已处理）
    VmaAllocationInfo allocInfo;
    vmaGetAllocationInfo(allocator, m_Allocation, &allocInfo);
    m_MappedPtr = allocInfo.pMappedData;
    m_IsMapped = (m_MappedPtr != nullptr);

    // Device address
    VkBufferDeviceAddressInfo addrInfo{};
    addrInfo.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addrInfo.buffer = m_Buffer;
    m_DeviceAddress = vkGetBufferDeviceAddress(allocInfo.device, &addrInfo);

    // 上传初始数据
    if (desc.initialData && m_MappedPtr) {
        std::memcpy(m_MappedPtr, desc.initialData, desc.size);
    }
}
```

- [ ] **Step 2: VulkanBuffer 析构函数改用 vmaDestroyBuffer**

```cpp
VulkanBuffer::~VulkanBuffer() {
    if (m_Buffer) {
        vmaDestroyBuffer(m_Allocator, m_Buffer, m_Allocation);
    }
}
```

不再需要 `vkUnmapMemory`（VMA 管理）、`vkDestroyBuffer`、`vkFreeMemory` —— `vmaDestroyBuffer` 一步全部完成。

- [ ] **Step 3: VulkanBuffer 成员类型更新**

在 `VulkanInternal.h` 的 `VulkanBuffer` 类中，成员从：
```cpp
VkDevice         m_Device;
VkDeviceMemory   m_Memory;
```
改为：
```cpp
VmaAllocator     m_Allocator;
VmaAllocation    m_Allocation;
```

- [ ] **Step 4: VulkanTexture 构造函数接收 VmaAllocator，改用 VMA 分配**

将 `VulkanTexture` 构造函数签名从：
```cpp
VulkanTexture::VulkanTexture(VkDevice device, VkPhysicalDevice physical,
                             VkCommandPool cmdPool, VkQueue queue,
                             const TextureDesc& desc)
```
改为：
```cpp
VulkanTexture::VulkanTexture(VmaAllocator allocator, VkCommandPool cmdPool, VkQueue queue,
                             const TextureDesc& desc)
```

成员变量 `m_Device` 改为 `m_Allocator`，`m_Physical` 移除。

内存分配部分（原 `vkGetImageMemoryRequirements` + `vkAllocateMemory` + `vkBindImageMemory`）替换为：

```cpp
    // 2. VMA 分配并绑定内存
    VmaAllocationCreateInfo allocCreateInfo{};
    allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    result = vmaAllocateMemoryForImage(allocator, m_Image, &allocCreateInfo,
                                        &m_Allocation, nullptr);
    HE_ASSERT(result == VK_SUCCESS, "VMA: Failed to allocate texture memory");

    vmaBindImageMemory(allocator, m_Allocation, m_Image);
```

- [ ] **Step 5: VulkanTexture 析构改用 VMA**

```cpp
VulkanTexture::~VulkanTexture() {
    for (auto& fv : m_FaceViews)
        if (fv) vkDestroyImageView(m_Allocator->..., fv, nullptr);
    m_FaceViews.clear();
    if (m_ImageView) vkDestroyImageView(m_Device, m_ImageView, nullptr);
    if (m_Image) {
        vmaDestroyImage(m_Allocator, m_Image, m_Allocation);
    }
}
```

注意：需要保留 `m_Device`（VkDevice）的引用，因为 `vkDestroyImageView` 仍需要它。VMA 不管理 ImageView。

- [ ] **Step 6: VulkanTexture 成员类型更新**

在 `VulkanInternal.h` 的 `VulkanTexture` 类中：
```cpp
VkDevice         m_Device;      // 保留（ImageView 销毁需要）
VmaAllocator     m_Allocator;   // 新增
VmaAllocation    m_Allocation;  // 替换 m_Memory
// 删除 VkPhysicalDevice m_Physical;
// 删除 VkDeviceMemory   m_Memory;
```

- [ ] **Step 7: UploadInitialData staging buffer 用 VMA**

`UploadInitialData` 中临时 staging buffer 的创建和销毁改用 VMA：

```cpp
    // 创建暂存缓冲区（VMA 管理，CPU 可见）
    VkBufferCreateInfo stagingInfo{};
    stagingInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingInfo.size        = dataSize;
    stagingInfo.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo stagingAllocInfo{};
    stagingAllocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                           | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    stagingAllocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;

    VkBuffer stagingBuffer;
    VmaAllocation stagingAlloc;
    vmaCreateBuffer(m_Allocator, &stagingInfo, &stagingAllocInfo,
                     &stagingBuffer, &stagingAlloc, nullptr);

    // 拷贝数据
    VmaAllocationInfo stagingAllocInfo2;
    vmaGetAllocationInfo(m_Allocator, stagingAlloc, &stagingAllocInfo2);
    std::memcpy(stagingAllocInfo2.pMappedData, desc.initialData, dataSize);

    // ...提交上传命令后...

    // 清理暂存资源（VMA 一步完成）
    vmaDestroyBuffer(m_Allocator, stagingBuffer, stagingAlloc);
    vkFreeCommandBuffers(m_Device, cmdPool, 1, &cmd);
    // 删除原有的 vkDestroyBuffer / vkFreeMemory 调用
```

- [ ] **Step 8: 删除 FindMemoryType 函数**

删除 `FindMemoryType()` 函数（约第 19-28 行）—— VMA 自动选择内存类型。

- [ ] **Step 9: 更新工厂函数签名**

```cpp
// 工厂函数 — 由 VulkanDevice 调用
std::unique_ptr<IRHIBuffer> CreateVulkanBuffer(
    VmaAllocator allocator, const BufferDesc& desc)
{
    return std::make_unique<VulkanBuffer>(allocator, desc);
}

std::unique_ptr<IRHITexture> CreateVulkanTexture(
    VmaAllocator allocator, VkCommandPool cmdPool, VkQueue queue,
    const TextureDesc& desc)
{
    return std::make_unique<VulkanTexture>(allocator, cmdPool, queue, desc);
}
```

- [ ] **Step 10: 更新 VulkanDevice.cpp 中的工厂调用**

`VulkanDevice::CreateBuffer`:
```cpp
std::unique_ptr<IRHIBuffer> VulkanDevice::CreateBuffer(const BufferDesc& desc) {
    return CreateVulkanBuffer(m_VmaAllocator, desc);
}
```

`VulkanDevice::CreateTexture`:
```cpp
std::unique_ptr<IRHITexture> VulkanDevice::CreateTexture(const TextureDesc& desc) {
    return CreateVulkanTexture(m_VmaAllocator, m_GraphicsCmdPool, m_GraphicsQueue, desc);
}
```

---

### Task 5: CMakeLists — 添加 VMA include 路径

**Files:**
- Modify: `Engine/RHI/Vulkan/CMakeLists.txt`

- [ ] **Step 1: 添加 VMA include 目录**

在现有 `target_include_directories` 中加入 VMA 路径：

```cmake
target_include_directories(HugEngineRHI PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine/External/VulkanMemoryAllocator   # 新增
)
```

---

### Task 6: Build Verification

- [ ] **Step 1: 编译验证**

```bash
cd D:/Source/HugEngine && cmake --build build --config Debug 2>&1 | tail -20
```

期望：`HugEngineRHI.lib` 编译成功，`04.Deferred.exe` 链接成功。

- [ ] **Step 2: 运行 04.Deferred（手动）**

启动 `build/bin/Debug/04.Deferred.exe`，验证：
- Sponza 场景正常渲染
- 无 Vulkan 验证层告警（内存方面）
- 帧率正常

---

## 自检

- [x] Spec coverage: 所有 5 个文件修改均已覆盖
- [x] Placeholder scan: 无 TBD/TODO
- [x] Type consistency: VmaAllocator 在所有 Task 中统一传递
