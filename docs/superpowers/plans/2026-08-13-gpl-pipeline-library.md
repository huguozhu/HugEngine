# Pipeline Library (Fast Link) + PSO 限流器 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 `VK_EXT_graphics_pipeline_library` 四段库缓存 + fast-link，并新增 PSO 创建限流器与 CVar 门控变体演示 Pass。

**Architecture:** 将 Graphics PSO 拆为 VertexInput / PreRaster / FragmentShader / FragmentOutput 四段独立库分别缓存，fast-link 组合完整 PSO；能力检测照 `VulkanDevice_MeshShader.cpp` 范式；限流器为 `IRHIDevice` 上的队列 + 每帧 drain。

**Tech Stack:** C++17 / Vulkan 1.4（`VK_EXT_graphics_pipeline_library` + 依赖 `VK_KHR_pipeline_library`）/ CMake (MSVC 2026)。

## Global Constraints

- **逐功能验证门控（用户硬性要求）**：每个任务只实现一个功能，**编译 + 运行验证通过后**，才允许开始下一个任务。任何任务未通过验证，不得继续。
- **Commit 规则（CLAUDE.md）**：不自动 `git commit`；提交前必须征得用户确认。commit log 用中文，不得包含 AI 信息（Co-Authored-By 等）。计划中的 commit 步骤在执行时须先向用户确认。
- **代码注释**：新增代码必须附中文注释。
- **命名**：新文件走 `VulkanDevice_GPL.cpp`、`VulkanPipelineLibrary.h/.cpp`；能力标志 `m_SupportsGPL` / `m_SupportsGPLFastLinking`；访问器 `SupportsGraphicsPipelineLibrary()` / `SupportsGPLFastLinking()`。
- **构建命令**：`cmake --build build --config Debug`（输出 `build/bin/Debug/04.Deferred.exe`）；单模块快速编译可用 `--target HugEngineRHI`。
- **运行验证命令**：`build/bin/Debug/04.Deferred.exe`（观察启动日志与 Validation 层报错）。
- **默认关闭**：`cvGPLVariantTest = 0`（默认关闭，设为 1 重新编译启用）。
- **关键正确性**：`graphicsPipelineLibraryFastLinking` 位于 **`VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT`**（Properties 结构，`vkGetPhysicalDeviceProperties2` 查询），**不在** Features 结构；`graphicsPipelineLibrary` 才在 **`VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT`**（Features 结构，`vkGetPhysicalDeviceFeatures2` 查询 + 进设备创建 pNext 链）。`VK_KHR_pipeline_library` 未核心化，必须显式启用。

---

### Task 1: GPL 能力检测与扩展启用

**Files:**
- Create: `Engine/RHI/Vulkan/VulkanDevice_GPL.cpp`
- Modify: `Engine/RHI/Vulkan/VulkanDevice.h`（访问器 + 声明 + 成员）
- Modify: `Engine/RHI/Vulkan/VulkanDevice.cpp`（`Initialize()` 挂载 + 扩展启用 + pNext 链）
- Modify: `Engine/RHI/CMakeLists.txt`（注册新源文件）

**Interfaces:**
- Consumes: 现有 `m_SupportsRT/Mesh/DGC` 范式、`QueryMeshCapabilities()` 调用点（`VulkanDevice.cpp:259-260`）。
- Produces: `bool SupportsGraphicsPipelineLibrary() const`、`bool SupportsGPLFastLinking() const`、`bool m_SupportsGPL`、`bool m_SupportsGPLFastLinking`（后续 Task 4 消费）。

- [ ] **Step 1: `VulkanDevice.h` 新增访问器与成员**

在 `SupportsDGC()`（`VulkanDevice.h:169`）之后新增：

```cpp
    // Graphics Pipeline Library 支持状态（fast-link 四段库拆分）
    bool    SupportsGraphicsPipelineLibrary() const { return m_SupportsGPL; }
    bool    SupportsGPLFastLinking() const { return m_SupportsGPLFastLinking; }
```

在私有声明区（`QueryDGCCapabilities();` 之后，`VulkanDevice.h:252` 附近）新增：

```cpp
    void QueryGPLCapabilities();
```

在成员区（`m_SupportsDGC` 之后，`VulkanDevice.h:289` 附近）新增：

```cpp
    bool             m_SupportsGPL            = false;
    bool             m_SupportsGPLFastLinking = false;
```

- [ ] **Step 2: 新建 `VulkanDevice_GPL.cpp`**

```cpp
// VulkanDevice_GPL.cpp — Graphics Pipeline Library 能力检测
// 照 VulkanDevice_MeshShader.cpp 的 QueryMeshCapabilities 范式，
// 查询 VK_EXT_graphics_pipeline_library 的 feature 与 properties。

#include "RHI/RHI.h"
#include "Core/Log.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "VulkanDevice.h"

#include <cstring>

namespace he::rhi {

void VulkanDevice::QueryGPLCapabilities() {
    m_SupportsGPL = false;
    m_SupportsGPLFastLinking = false;

    // 1. 检查设备扩展是否可用
    u32 extCount = 0;
    vkEnumerateDeviceExtensionProperties(m_Physical, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extCount);
    vkEnumerateDeviceExtensionProperties(m_Physical, nullptr, &extCount, extensions.data());

    bool hasGPL = false;
    for (auto& ext : extensions) {
        if (strcmp(ext.extensionName, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) == 0) {
            hasGPL = true;
            break;
        }
    }
    if (!hasGPL) {
        HE_CORE_INFO("Graphics Pipeline Library: 不支持（缺少 VK_EXT_graphics_pipeline_library）");
        return;
    }

    // 2. 查询 feature（graphicsPipelineLibrary 在 Features 结构）
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gplFeatures{};
    gplFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 feat2{};
    feat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feat2.pNext = &gplFeatures;
    vkGetPhysicalDeviceFeatures2(m_Physical, &feat2);
    if (!gplFeatures.graphicsPipelineLibrary) {
        HE_CORE_INFO("Graphics Pipeline Library: feature 不可用");
        return;
    }
    m_SupportsGPL = true;

    // 3. 查询 properties（graphicsPipelineLibraryFastLinking 在 Properties 结构）
    VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gplProps{};
    gplProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &gplProps;
    vkGetPhysicalDeviceProperties2(m_Physical, &props2);
    m_SupportsGPLFastLinking = gplProps.graphicsPipelineLibraryFastLinking;

    HE_CORE_INFO("Graphics Pipeline Library: 硬件支持已检测 (fastLinking={})",
                 m_SupportsGPLFastLinking ? 1 : 0);
}

} // namespace he::rhi
```

- [ ] **Step 3: `VulkanDevice.cpp` 挂载 `QueryGPLCapabilities()`**

在 `Initialize()` 的能力查询区（`VulkanDevice.cpp:260` `QueryDGCCapabilities();` 之后）新增：

```cpp
    QueryGPLCapabilities();      // → VulkanDevice_GPL.cpp
```

- [ ] **Step 4: `VulkanDevice.cpp` 启用扩展 + pNext 链**

在 `CreateLogicalDevice()` 的 DGC 块之后（`VulkanDevice.cpp:452` 附近，`m_SupportsDGC` 块之后）新增 feature 结构声明与条件扩展：

```cpp
    // 条件启用 Graphics Pipeline Library 扩展（fast-link 依赖 VK_KHR_pipeline_library）
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gplFeature{};
    gplFeature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT;
    gplFeature.graphicsPipelineLibrary = VK_TRUE;

    if (m_SupportsGPL) {
        deviceExtensions.push_back(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);      // 依赖扩展
        deviceExtensions.push_back(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
        HE_CORE_INFO("Graphics Pipeline Library 扩展已启用");
    }
```

在 pNext 链末尾（`VulkanDevice.cpp:535` `if (m_SupportsDGC) {...}` 块之后、`if (hasDerivatives)` 之前）新增：

```cpp
    if (m_SupportsGPL) {
        *ppNext = &gplFeature; ppNext = &gplFeature.pNext;
    }
```

- [ ] **Step 5: `CMakeLists.txt` 注册新源文件**

在 `RHI_VULKAN_SOURCES`（`CMakeLists.txt:46-62`）中 `Vulkan/VulkanDevice_MeshShader.cpp` 之后新增：

```cmake
        Vulkan/VulkanDevice_GPL.cpp
```

- [ ] **Step 6: 编译 + 运行验证**

Run: `cmake --build build --config Debug`
Expected: 编译通过（RHI 全模块零错误）。

Run: `build/bin/Debug/04.Deferred.exe`（启动后观察日志后退出）
Expected: 启动日志出现 `Graphics Pipeline Library: 硬件支持已检测 (fastLinking=1)`；无新增 Validation 错误；现有渲染正常。

- [ ] **Step 7: Commit（先向用户确认）**

```bash
git add Engine/RHI/Vulkan/VulkanDevice_GPL.cpp Engine/RHI/Vulkan/VulkanDevice.h Engine/RHI/Vulkan/VulkanDevice.cpp Engine/RHI/CMakeLists.txt
git commit -m "RHI: 新增 GPL 能力检测与扩展启用"
```

---

### Task 2: PipelineLibraryCache 四段库缓存（新文件，暂未集成）

**Files:**
- Create: `Engine/RHI/Vulkan/VulkanPipelineLibrary.h`
- Create: `Engine/RHI/Vulkan/VulkanPipelineLibrary.cpp`
- Modify: `Engine/RHI/CMakeLists.txt`（注册新源文件）

**Interfaces:**
- Consumes: `PipelineStateDesc`（`RHI/Shader.h`）、`DeferredDestructionQueue`、`kMaxColorAttachments`（`RHI/Types.h`）。
- Produces: `struct GraphicsPipelineParts`、`enum class PipelinePartKind`、`uint64_t HashPipelinePart(const PipelineStateDesc&, PipelinePartKind)`、`class PipelineLibraryCache`（`Initialize` / `Shutdown` / `GetOrCreateVertexInputLibrary` / `GetOrCreatePreRasterLibrary` / `GetOrCreateFragmentShaderLibrary` / `GetOrCreateFragmentOutputLibrary` / `LinkPipeline`）——后续 Task 4 消费。

- [ ] **Step 1: 新建 `VulkanPipelineLibrary.h`**

```cpp
#pragma once

// ============================================================
// VulkanPipelineLibrary.h — Graphics Pipeline Library (GPL)
// 将 Graphics PSO 拆分为 4 段独立库并分别缓存：变体场景下
// 只重编变化段，其余段复用，fast-link 组合完整 PSO
// （~0.5ms vs 单片 50ms）。依赖 VK_EXT_graphics_pipeline_library。
// ============================================================

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "RHI/Shader.h"

#include <array>
#include <unordered_map>
#include <vector>

namespace he::rhi {

class DeferredDestructionQueue;

// 四段库类型
enum class PipelinePartKind {
    VertexInput,
    PreRaster,
    FragmentShader,
    FragmentOutput,
};

// 分解后的图形管线状态（传统 VS+FS 路径），单片与 GPL 共享
struct GraphicsPipelineParts {
    // 由 create info 指针指向的持久数组（保持指针有效到 vkCreate 完成）
    std::array<VkAttachmentDescription, kMaxColorAttachments + 1> attachments{};
    std::array<VkAttachmentReference,   kMaxColorAttachments>     colorRefs{};
    std::array<VkPipelineColorBlendAttachmentState, kMaxColorAttachments> blendStates{};
    std::vector<VkVertexInputAttributeDescription> attrs;
    VkVertexInputBindingDescription binding{};
    std::vector<VkPushConstantRange> pushRanges;

    VkPipelineVertexInputStateCreateInfo   vertexInput{};
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    VkPipelineViewportStateCreateInfo      viewportState{};
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    VkPipelineMultisampleStateCreateInfo   multisample{};
    VkPipelineDepthStencilStateCreateInfo  depthStencil{};
    VkPipelineColorBlendStateCreateInfo    colorBlend{};
    VkPipelineDynamicStateCreateInfo       dynState{};
    VkPipelineShaderStageCreateInfo        vsStage{};
    VkPipelineShaderStageCreateInfo        fsStage{};

    VkPipelineLayout layout     = VK_NULL_HANDLE;
    VkRenderPass     renderPass = VK_NULL_HANDLE;
    u32              subpass    = 0;

    // 临时着色器模块（路径末端由调用方销毁）
    VkShaderModule vs = VK_NULL_HANDLE;
    VkShaderModule fs = VK_NULL_HANDLE;
};

// 计算单段库的哈希（仅该段相关字段）
uint64_t HashPipelinePart(const PipelineStateDesc& desc, PipelinePartKind kind);

// 四段库缓存 + fast-link
class PipelineLibraryCache {
public:
    void Initialize(VkDevice device, VkPipelineCache cache, DeferredDestructionQueue* ddq);
    void Shutdown();

    // 四段：查/建（缺失时用 VK_PIPELINE_CREATE_LIBRARY_BIT_KHR 创建并缓存）
    VkPipeline GetOrCreateVertexInputLibrary(u64 hash, const GraphicsPipelineParts& p);
    VkPipeline GetOrCreatePreRasterLibrary(u64 hash, const GraphicsPipelineParts& p);
    VkPipeline GetOrCreateFragmentShaderLibrary(u64 hash, const GraphicsPipelineParts& p);
    VkPipeline GetOrCreateFragmentOutputLibrary(u64 hash, const GraphicsPipelineParts& p);

    // fast-link：组合 4 段产出完整 VkPipeline
    // @param linkTimeOptimize  fast-linking 不可用时置 true（带 LTO bit）
    VkPipeline LinkPipeline(const VkPipeline libs[4], const GraphicsPipelineParts& p,
                            bool linkTimeOptimize);

private:
    VkDevice                m_Device = VK_NULL_HANDLE;
    VkPipelineCache         m_Cache  = VK_NULL_HANDLE;
    DeferredDestructionQueue* m_DDQ  = nullptr;

    std::unordered_map<u64, VkPipeline> m_VertexInputLibs;
    std::unordered_map<u64, VkPipeline> m_PreRasterLibs;
    std::unordered_map<u64, VkPipeline> m_FragmentShaderLibs;
    std::unordered_map<u64, VkPipeline> m_FragmentOutputLibs;
};

} // namespace he::rhi
```

- [ ] **Step 2: 新建 `VulkanPipelineLibrary.cpp`**

```cpp
#include "VulkanPipelineLibrary.h"
#include "DeferredDestructionQueue.h"
#include "Core/Log.h"

namespace he::rhi {

// ============================================================
// FNV-1a 哈希辅助（与 VulkanPipeline.cpp 的 HashPipelineStateDesc 一致）
// ============================================================
namespace {
uint64_t FnvHashBytes(uint64_t h, const void* data, size_t size) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) { h ^= bytes[i]; h *= 0x100000001b3ULL; }
    return h;
}
uint64_t FnvHashU32(uint64_t h, u32 v) { return FnvHashBytes(h, &v, sizeof(v)); }
uint64_t FnvHashShader(uint64_t h, const ShaderBytecode* bc) {
    if (!bc) return h;
    return FnvHashBytes(h, bc->spirv.data(), bc->spirv.size() * sizeof(u32));
}
} // namespace

// 单段库哈希：仅纳入该段相关的 PipelineStateDesc 字段
uint64_t HashPipelinePart(const PipelineStateDesc& desc, PipelinePartKind kind) {
    uint64_t h = 0xcbf29ce484222325ULL;
    switch (kind) {
    case PipelinePartKind::VertexInput:
        h = FnvHashU32(h, desc.vertexLayout.stride);
        for (auto& a : desc.vertexLayout.attributes) {
            h = FnvHashU32(h, a.location);
            h = FnvHashU32(h, a.binding);
            h = FnvHashU32(h, static_cast<u32>(a.format));
            h = FnvHashU32(h, a.offset);
        }
        break;
    case PipelinePartKind::PreRaster:
        h = FnvHashShader(h, desc.vertexShader);
        h = FnvHashU32(h, static_cast<u32>(desc.topology));
        h = FnvHashU32(h, static_cast<u32>(desc.cullMode));
        h = FnvHashU32(h, static_cast<u32>(desc.frontFace));
        h = FnvHashU32(h, static_cast<u32>(desc.fillMode));
        h = FnvHashU32(h, desc.depthTest ? 1u : 0u);
        h = FnvHashU32(h, desc.depthWrite ? 1u : 0u);
        h = FnvHashU32(h, static_cast<u32>(desc.depthCompare));
        for (auto& pc : desc.pushConstantRanges) {
            h = FnvHashU32(h, pc.stageMask);
            h = FnvHashU32(h, pc.offset);
            h = FnvHashU32(h, pc.size);
        }
        break;
    case PipelinePartKind::FragmentShader:
        h = FnvHashShader(h, desc.pixelShader);
        for (auto& pc : desc.pushConstantRanges) {
            h = FnvHashU32(h, pc.stageMask);
            h = FnvHashU32(h, pc.offset);
            h = FnvHashU32(h, pc.size);
        }
        break;
    case PipelinePartKind::FragmentOutput:
        h = FnvHashU32(h, desc.colorAttachmentCount);
        for (u32 i = 0; i < desc.colorAttachmentCount; ++i)
            h = FnvHashU32(h, static_cast<u32>(desc.colorFormats[i]));
        h = FnvHashU32(h, static_cast<u32>(desc.depthFormat));
        for (u32 i = 0; i < desc.colorAttachmentCount; ++i) {
            const auto& cb = desc.colorBlend[i];
            h = FnvHashU32(h, cb.blendEnable ? 1u : 0u);
            h = FnvHashU32(h, static_cast<u32>(cb.srcColorBlendFactor));
            h = FnvHashU32(h, static_cast<u32>(cb.dstColorBlendFactor));
            h = FnvHashU32(h, static_cast<u32>(cb.colorBlendOp));
            h = FnvHashU32(h, static_cast<u32>(cb.writeMask));
        }
        h = FnvHashU32(h, desc.sampleCount);
        h = FnvHashU32(h, desc.subpassIndex);
        break;
    }
    return h;
}

// ============================================================
// PipelineLibraryCache 实现
// ============================================================
void PipelineLibraryCache::Initialize(VkDevice device, VkPipelineCache cache,
                                      DeferredDestructionQueue* ddq) {
    m_Device = device;
    m_Cache  = cache;
    m_DDQ    = ddq;
}

void PipelineLibraryCache::Shutdown() {
    auto destroyAll = [&](std::unordered_map<u64, VkPipeline>& map) {
        for (auto& [hash, lib] : map) {
            (void)hash;
            if (lib != VK_NULL_HANDLE && m_DDQ) {
                VkDevice dev = m_Device;
                m_DDQ->Enqueue([dev, lib]() { vkDestroyPipeline(dev, lib, nullptr); });
            }
        }
        map.clear();
    };
    destroyAll(m_VertexInputLibs);
    destroyAll(m_PreRasterLibs);
    destroyAll(m_FragmentShaderLibs);
    destroyAll(m_FragmentOutputLibs);
}

// 通用：构造带 LIBRARY_BIT 的部分管线 create info 并创建单个库段
static VkPipeline CreateLibrarySegment(
    VkDevice device, VkPipelineCache cache, VkGraphicsPipelineLibraryFlagsEXT flags,
    const GraphicsPipelineParts& p,
    VkPipelineShaderStageCreateInfo* stages, u32 stageCount,
    const VkPipelineVertexInputStateCreateInfo* vi,
    const VkPipelineInputAssemblyStateCreateInfo* ia,
    const VkPipelineViewportStateCreateInfo* vp,
    const VkPipelineRasterizationStateCreateInfo* rs,
    const VkPipelineMultisampleStateCreateInfo* ms,
    const VkPipelineDepthStencilStateCreateInfo* ds,
    const VkPipelineColorBlendStateCreateInfo* cb,
    const VkPipelineDynamicStateCreateInfo* dyn,
    VkPipelineLayout layout, VkRenderPass renderPass, u32 subpass) {

    VkGraphicsPipelineLibraryCreateInfoEXT libInfo{};
    libInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT;
    libInfo.flags = flags;

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext               = &libInfo;
    ci.flags               = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
    ci.stageCount          = stageCount;
    ci.pStages             = stages;
    ci.pVertexInputState   = vi;
    ci.pInputAssemblyState = ia;
    ci.pViewportState      = vp;
    ci.pRasterizationState = rs;
    ci.pMultisampleState   = ms;
    ci.pDepthStencilState  = ds;
    ci.pColorBlendState    = cb;
    ci.pDynamicState       = dyn;
    ci.layout              = layout;
    ci.renderPass          = renderPass;
    ci.subpass             = subpass;

    VkPipeline lib = VK_NULL_HANDLE;
    VkResult vr = vkCreateGraphicsPipelines(device, cache, 1, &ci, nullptr, &lib);
    if (vr != VK_SUCCESS) {
        HE_CORE_WARN("PipelineLibraryCache: 库段创建失败 (flags=0x{:x}, result={})",
                     static_cast<u32>(flags), static_cast<i32>(vr));
    }
    return lib;
}

VkPipeline PipelineLibraryCache::GetOrCreateVertexInputLibrary(u64 hash, const GraphicsPipelineParts& p) {
    auto it = m_VertexInputLibs.find(hash);
    if (it != m_VertexInputLibs.end()) return it->second;
    VkPipeline lib = CreateLibrarySegment(
        m_Device, m_Cache, VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, p,
        nullptr, 0, &p.vertexInput, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        VK_NULL_HANDLE, VK_NULL_HANDLE, 0);
    if (lib != VK_NULL_HANDLE) m_VertexInputLibs[hash] = lib;
    return lib;
}

VkPipeline PipelineLibraryCache::GetOrCreatePreRasterLibrary(u64 hash, const GraphicsPipelineParts& p) {
    auto it = m_PreRasterLibs.find(hash);
    if (it != m_PreRasterLibs.end()) return it->second;
    VkPipelineShaderStageCreateInfo stage = p.vsStage;
    VkPipeline lib = CreateLibrarySegment(
        m_Device, m_Cache, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, p,
        &stage, 1, nullptr, &p.inputAssembly, &p.viewportState, &p.rasterizer,
        nullptr, &p.depthStencil, nullptr, &p.dynState,
        p.layout, VK_NULL_HANDLE, 0);
    if (lib != VK_NULL_HANDLE) m_PreRasterLibs[hash] = lib;
    return lib;
}

VkPipeline PipelineLibraryCache::GetOrCreateFragmentShaderLibrary(u64 hash, const GraphicsPipelineParts& p) {
    auto it = m_FragmentShaderLibs.find(hash);
    if (it != m_FragmentShaderLibs.end()) return it->second;
    VkPipelineShaderStageCreateInfo stage = p.fsStage;
    VkPipeline lib = CreateLibrarySegment(
        m_Device, m_Cache, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, p,
        &stage, 1, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        p.layout, VK_NULL_HANDLE, 0);
    if (lib != VK_NULL_HANDLE) m_FragmentShaderLibs[hash] = lib;
    return lib;
}

VkPipeline PipelineLibraryCache::GetOrCreateFragmentOutputLibrary(u64 hash, const GraphicsPipelineParts& p) {
    auto it = m_FragmentOutputLibs.find(hash);
    if (it != m_FragmentOutputLibs.end()) return it->second;
    VkPipeline lib = CreateLibrarySegment(
        m_Device, m_Cache, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT, p,
        nullptr, 0, nullptr, nullptr, nullptr, nullptr, &p.multisample, nullptr, &p.colorBlend,
        nullptr, p.layout, p.renderPass, p.subpass);
    if (lib != VK_NULL_HANDLE) m_FragmentOutputLibs[hash] = lib;
    return lib;
}

VkPipeline PipelineLibraryCache::LinkPipeline(const VkPipeline libs[4], const GraphicsPipelineParts& p,
                                              bool linkTimeOptimize) {
    VkGraphicsPipelineLibraryCreateInfoEXT libInfo{};
    libInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT;
    libInfo.flags = 0;  // 链接而非创建库段

    VkPipelineLibraryCreateInfoKHR linkInfo{};
    linkInfo.sType        = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR;
    linkInfo.pNext        = &libInfo;
    linkInfo.libraryCount = 4;
    linkInfo.pLibraries   = libs;

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType     = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext     = &linkInfo;
    ci.flags     = linkTimeOptimize ? VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT : 0;
    ci.layout    = p.layout;
    ci.renderPass = p.renderPass;
    ci.subpass   = p.subpass;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult vr = vkCreateGraphicsPipelines(m_Device, m_Cache, 1, &ci, nullptr, &pipeline);
    if (vr != VK_SUCCESS) {
        HE_CORE_WARN("PipelineLibraryCache: fast-link 失败 (result={})", static_cast<i32>(vr));
    }
    return pipeline;
}

} // namespace he::rhi
```

- [ ] **Step 3: `CMakeLists.txt` 注册新源文件**

在 `RHI_VULKAN_SOURCES` 中 `Vulkan/VulkanPipeline.cpp` 之后新增：

```cmake
        Vulkan/VulkanPipelineLibrary.cpp
```

- [ ] **Step 4: 编译验证**

Run: `cmake --build build --config Debug --target HugEngineRHI`
Expected: 编译通过（新文件零错误；暂未集成，无运行期行为变化）。

- [ ] **Step 5: Commit（先向用户确认）**

```bash
git add Engine/RHI/Vulkan/VulkanPipelineLibrary.h Engine/RHI/Vulkan/VulkanPipelineLibrary.cpp Engine/RHI/CMakeLists.txt
git commit -m "RHI: 新增 PipelineLibraryCache 四段库缓存"
```

---

### Task 3: 重构单片路径为共享 GraphicsPipelineParts（行为不变）

**Files:**
- Modify: `Engine/RHI/Vulkan/VulkanPipeline.cpp`（提取 `BuildGraphicsPipelineParts`，重写传统 VS+FS 单片路径）

**Interfaces:**
- Consumes: `GraphicsPipelineParts`（Task 2）、`ToVkFormat`/`ToVkLoadOp`/`ToVkCullMode` 等（`VulkanRT.h`）。
- Produces: `static bool BuildGraphicsPipelineParts(VkDevice, const PipelineStateDesc&, const std::vector<VkDescriptorSetLayout>&, GraphicsPipelineParts&)`（Task 4 消费）。

- [ ] **Step 1: 提取 `BuildGraphicsPipelineParts`**

将 `VulkanPipeline.cpp` 传统 VS+FS 路径（`CreateVulkanPipeline` 中第 461 行 `VkShaderModule vert = ...` 至第 684 行 `stages[1].pName = ...`）的整体状态构建逻辑，提取为文件级静态函数，填充 `GraphicsPipelineParts`：

```cpp
// 构建传统 VS+FS 图形管线的全部分解状态（单片与 GPL 路径共享）
// 原 CreateVulkanPipeline 中 461~684 行的状态构建逻辑原样搬入，仅将
// 栈数组替换为 out.attachments / out.colorRefs / out.blendStates / out.attrs，
// 将 renderPass / layout / shader modules 写入 out 字段。
static bool BuildGraphicsPipelineParts(VkDevice device, const PipelineStateDesc& desc,
                                       const std::vector<VkDescriptorSetLayout>& descLayouts,
                                       GraphicsPipelineParts& out);
```

搬入内容要点（逐行对应原 461~684 行，保持逻辑完全一致）：
- `out.vs` / `out.fs` = `createShader(...)`（原 461-462）
- 颜色附件 `colorAttachments[c]` → `out.attachments[c]`、`colorRefs[c]` → `out.colorRefs[c]`（原 470-487）
- 深度附件 `depthAttach` → 写入 `out.attachments[colorAttachmentCount]`（原 489-499）
- subpass / attachments 数组 / dependency → `vkCreateRenderPass` 写入 `out.renderPass`（原 505-538）
- 顶点输入：`binding` → `out.binding`、`vkAttrs` → `out.attrs`、`vertexInput` → `out.vertexInput`（原 541-591）
- `inputAssembly` → `out.inputAssembly`（原 594-596）
- `viewportState` → `out.viewportState`（原 599-602）
- `rasterizer` → `out.rasterizer`（原 604-609）
- `ms` → `out.multisample`（原 611-613）
- `depthStencil` → `out.depthStencil`（原 616-622）
- `blendAttachments[c]` → `out.blendStates[c]`、`colorBlend` → `out.colorBlend`（原 624-640）
- `dynState` → `out.dynState`（原 642-646）
- push ranges → `out.pushRanges`、`vkCreatePipelineLayout` → `out.layout`（原 649-673）
- `stages[0]/[1]` → `out.vsStage` / `out.fsStage`（原 676-684）

- [ ] **Step 2: 重写单片路径消费 `parts`**

将原 686~724 行替换为：

```cpp
    // 组装完整单片 pipeline
    VkPipelineShaderStageCreateInfo stages[2] = { parts.vsStage, parts.fsStage };
    VkGraphicsPipelineCreateInfo pipeInfo{};
    pipeInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeInfo.stageCount          = 2;
    pipeInfo.pStages             = stages;
    pipeInfo.pVertexInputState   = &parts.vertexInput;
    pipeInfo.pInputAssemblyState = &parts.inputAssembly;
    pipeInfo.pViewportState      = &parts.viewportState;
    pipeInfo.pRasterizationState = &parts.rasterizer;
    pipeInfo.pMultisampleState   = &parts.multisample;
    pipeInfo.pDepthStencilState  = &parts.depthStencil;
    pipeInfo.pColorBlendState    = &parts.colorBlend;
    pipeInfo.pDynamicState       = &parts.dynState;
    pipeInfo.layout              = parts.layout;
    pipeInfo.renderPass          = parts.renderPass;
    pipeInfo.subpass             = 0;

    VkPipeline pipeline;
    vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipeInfo, nullptr, &pipeline);

    vkDestroyShaderModule(device, parts.vs, nullptr);
    vkDestroyShaderModule(device, parts.fs, nullptr);
```

（`device`、`pipelineCache` 为原函数已有局部变量；后续「插入 PSO 缓存」逻辑不变。）

- [ ] **Step 3: 编译 + 运行验证（行为不变）**

Run: `cmake --build build --config Debug`
Expected: 编译通过。

Run: `build/bin/Debug/04.Deferred.exe`（启动后观察日志后退出）
Expected: 现有 PSO 创建日志正常（`Vulkan graphics pipeline created` / `Vulkan mesh shader pipeline created` / `Vulkan compute pipeline created`），渲染画面与重构前一致，零新增 Validation 错误。此步骤确认重构无回归。

- [ ] **Step 4: Commit（先向用户确认）**

```bash
git add Engine/RHI/Vulkan/VulkanPipeline.cpp
git commit -m "RHI: 重构单片管线构建为共享 GraphicsPipelineParts"
```

---

### Task 4: 集成 GPL fast-link 到 CreateVulkanPipeline

**Files:**
- Modify: `Engine/RHI/Vulkan/VulkanDevice.h`（`m_PipelineLibraryCache` 成员 + `GetPipelineLibraryCache()` 访问器）
- Modify: `Engine/RHI/Vulkan/VulkanDevice.cpp`（`Initialize()`/`Shutdown()` 生命周期）
- Modify: `Engine/RHI/Vulkan/VulkanPipeline.cpp`（GPL 分支）

**Interfaces:**
- Consumes: `PipelineLibraryCache`、`HashPipelinePart`、`GraphicsPipelineParts`（Task 2）、`BuildGraphicsPipelineParts`（Task 3）、`SupportsGraphicsPipelineLibrary()`/`SupportsGPLFastLinking()`（Task 1）。
- Produces: GPL fast-link 创建路径（后续 Task 6 演示消费）。

- [ ] **Step 1: `VulkanDevice.h` 新增成员与访问器**

在 `PSOPrecompileManager` 成员之后（`VulkanDevice.h:275` 附近）新增：

```cpp
    // ============================================================
    // Graphics Pipeline Library — 四段库缓存 + fast-link
    // ============================================================
    PipelineLibraryCache m_PipelineLibraryCache;
```

在 `GetPipelineCache()` 访问器之后（`VulkanDevice.h:195` 附近）新增：

```cpp
    /// PipelineLibraryCache 访问器（供 CreateVulkanPipeline 使用）
    PipelineLibraryCache& GetPipelineLibraryCache() { return m_PipelineLibraryCache; }
```

并在 include 区（`#include "PSOPrecompileManager.h"` 之后，`VulkanDevice.h:26` 附近）新增：

```cpp
#include "VulkanPipelineLibrary.h"
```

- [ ] **Step 2: `VulkanDevice.cpp` 生命周期**

`Initialize()` 中（`m_PSOPrecompileManager.Initialize(...)` 之后，`VulkanDevice.cpp:276` 附近）新增：

```cpp
    // 11. 初始化 PipelineLibraryCache（GPL 四段库缓存）
    m_PipelineLibraryCache.Initialize(m_Device, m_PipelineCache, &m_DeferredDestroy);
```

`Shutdown()` 中（`m_PSOPrecompileManager.Shutdown();` 之后，`VulkanDevice.cpp:295` 附近）新增：

```cpp
    // 1.8. 销毁 PipelineLibraryCache（延迟销毁四段库 VkPipeline）
    m_PipelineLibraryCache.Shutdown();
```

- [ ] **Step 3: `VulkanPipeline.cpp` 插入 GPL 分支**

在传统 VS+FS 路径中 `BuildGraphicsPipelineParts` 成功之后（当前 `VulkanPipeline.cpp:705` 的 `}` 之后、`// 组装完整单片 pipeline` 之前）插入 GPL 分支。**复用同一个 `parts`**（不再二次调用 `BuildGraphicsPipelineParts`，避免回退时 renderPass/layout 泄漏与 vs/fs 双销毁）：

```cpp
    // ── GPL fast-link 分支（支持 GPL 时优先；任一段创建或 link 失败则回退单片路径）──
    if (vulkanDevice && vulkanDevice->SupportsGraphicsPipelineLibrary()) {
        u64 hVI = HashPipelinePart(desc, PipelinePartKind::VertexInput);
        u64 hPR = HashPipelinePart(desc, PipelinePartKind::PreRaster);
        u64 hFS = HashPipelinePart(desc, PipelinePartKind::FragmentShader);
        u64 hFO = HashPipelinePart(desc, PipelinePartKind::FragmentOutput);

        auto& libCache = vulkanDevice->GetPipelineLibraryCache();
        VkPipeline libs[4] = {
            libCache.GetOrCreateVertexInputLibrary(hVI, parts),
            libCache.GetOrCreatePreRasterLibrary(hPR, parts),
            libCache.GetOrCreateFragmentShaderLibrary(hFS, parts),
            libCache.GetOrCreateFragmentOutputLibrary(hFO, parts),
        };

        if (libs[0] && libs[1] && libs[2] && libs[3]) {
            VkPipeline pipeline = libCache.LinkPipeline(
                libs, parts, /*linkTimeOptimize=*/!vulkanDevice->SupportsGPLFastLinking());
            if (pipeline != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device, parts.vs, nullptr);
                vkDestroyShaderModule(device, parts.fs, nullptr);

                HE_CORE_INFO("Vulkan graphics pipeline created via GPL fast-link");
                uint64_t hash = HashPipelineStateDesc(desc);
                vulkanDevice->InsertPSOToCache(hash, pipeline, parts.layout, parts.renderPass);
                VkPipeline cp = VK_NULL_HANDLE; VkPipelineLayout cl = VK_NULL_HANDLE;
                VkRenderPass cr = VK_NULL_HANDLE;
                auto ref = vulkanDevice->GetCachedPSORef(hash, cp, cl, cr);
                if (ref) {
                    return std::make_unique<VulkanPipelineState>(
                        device, pipeline, parts.layout, parts.renderPass,
                        VK_PIPELINE_BIND_POINT_GRAPHICS, ref,
                        &vulkanDevice->GetDeferredDestroy());
                }
                return std::make_unique<VulkanPipelineState>(
                    device, pipeline, parts.layout, parts.renderPass,
                    VK_PIPELINE_BIND_POINT_GRAPHICS);
            }
        }
        // 任一段创建失败或 link 失败 → 回退下方单片路径（复用同一个 parts，无泄漏）
    }
```

- [ ] **Step 4: 编译 + 运行验证**

Run: `cmake --build build --config Debug`
Expected: 编译通过。

Run: `build/bin/Debug/04.Deferred.exe`（启动后观察日志后退出）
Expected: 传统 VS+FS PSO 创建日志变为 `Vulkan graphics pipeline created via GPL fast-link`；Compute/Mesh PSO 仍走单片路径；画面正常；零 Validation 错误。

- [ ] **Step 5: Commit（先向用户确认）**

```bash
git add Engine/RHI/Vulkan/VulkanDevice.h Engine/RHI/Vulkan/VulkanDevice.cpp Engine/RHI/Vulkan/VulkanPipeline.cpp
git commit -m "RHI: 集成 GPL fast-link 到 CreateVulkanPipeline"
```

---

### Task 5: PSO 创建限流器

**Files:**
- Modify: `Engine/RHI/RHI/RHI.h`（3 个虚方法）
- Modify: `Engine/RHI/Vulkan/VulkanDevice.h`（队列成员 + override）
- Modify: `Engine/RHI/Vulkan/VulkanDevice.cpp`（3 方法实现）

**Interfaces:**
- Consumes: `CreatePipelineState`（现有）。
- Produces: `EnqueuePSOCreate(const PipelineStateDesc&)`、`ProcessPSOCreateQueue(u32) -> std::vector<std::unique_ptr<IRHIPipelineState>>`、`GetPendingPSOCreateCount() const`（Task 6 消费）。

- [ ] **Step 1: `RHI/RHI.h` 新增虚方法**

在 PSO 预热管理器虚方法之后（`RHI.h:64` 附近）新增：

```cpp
    // --- PSO 创建限流器 ---
    // 入队一个 PSO 描述符到限流队列（非阻塞，帧循环内不直接创建）
    virtual void EnqueuePSOCreate(const PipelineStateDesc& desc) {}
    // 每帧从队列取最多 maxPerFrame 个创建，返回本次创建的 PSO（供调用方持有）
    virtual std::vector<std::unique_ptr<IRHIPipelineState>> ProcessPSOCreateQueue(u32 maxPerFrame) { return {}; }
    // 队列中待创建的 PSO 数量
    virtual u32 GetPendingPSOCreateCount() const { return 0; }
```

- [ ] **Step 2: `VulkanDevice.h` 新增 override + 队列成员**

在 public 区 PSO 预热 override 之后（`VulkanDevice.h:69` 附近）新增：

```cpp
    // PSO 创建限流器 — 帧循环内按限流批量创建 PSO（避免单帧卡顿）
    void EnqueuePSOCreate(const PipelineStateDesc& desc) override;
    std::vector<std::unique_ptr<IRHIPipelineState>> ProcessPSOCreateQueue(u32 maxPerFrame) override;
    u32 GetPendingPSOCreateCount() const override;
```

在 private 成员区（`m_PSOPrecompileManager` 之后）新增：

```cpp
    // PSO 创建限流队列（主线程写入 → NextFrame 逐帧 drain）
    std::deque<PipelineStateDesc> m_PendingCreates;
```

并在 include 区补 `<deque>`（若无则新增 `#include <deque>`）。

- [ ] **Step 3: `VulkanDevice.cpp` 实现**

在 PSO 预热管理器实现之后（`VulkanDevice.cpp:1067` 附近）新增：

```cpp
// ============================================================
// PSO 创建限流器 — 帧循环内按限流批量创建 PSO
// ============================================================
void VulkanDevice::EnqueuePSOCreate(const PipelineStateDesc& desc) {
    m_PendingCreates.push_back(desc);
}

std::vector<std::unique_ptr<IRHIPipelineState>> VulkanDevice::ProcessPSOCreateQueue(u32 maxPerFrame) {
    std::vector<std::unique_ptr<IRHIPipelineState>> created;
    u32 processed = 0;
    while (processed < maxPerFrame && !m_PendingCreates.empty()) {
        PipelineStateDesc desc = m_PendingCreates.front();
        m_PendingCreates.pop_front();
        // 创建 PSO（内部走 GPL fast-link 或单片路径，取决于能力与 desc）
        created.push_back(CreatePipelineState(desc));
        processed++;
    }
    return created;
}

u32 VulkanDevice::GetPendingPSOCreateCount() const {
    return static_cast<u32>(m_PendingCreates.size());
}
```

- [ ] **Step 4: 编译验证**

Run: `cmake --build build --config Debug`
Expected: 编译通过（限流器暂无调用方，无运行期行为变化）。

- [ ] **Step 5: Commit（先向用户确认）**

```bash
git add Engine/RHI/RHI/RHI.h Engine/RHI/Vulkan/VulkanDevice.h Engine/RHI/Vulkan/VulkanDevice.cpp
git commit -m "RHI: 新增 PSO 创建限流器"
```

---

### Task 6: GPL 变体演示 Pass + NextFrame 限流 drain

**Files:**
- Modify: `Engine/RHI/RHI/Types.h`（`DeviceCaps` 新增 `supportsGraphicsPipelineLibrary`）
- Modify: `Engine/RHI/Vulkan/VulkanDevice.cpp`（`GetCaps()` 填充该字段）
- Modify: `Engine/Render/Pipeline/DeferredPipeline.h`（持有变体 PSO 的成员）
- Modify: `Engine/Render/Pipeline/DeferredPipeline.cpp`（CVar 静态量 + 初始化入队 + `NextFrame()` drain）

**Interfaces:**
- Consumes: `EnqueuePSOCreate` / `ProcessPSOCreateQueue` / `GetPendingPSOCreateCount`（Task 5）、`GetCaps().supportsGraphicsPipelineLibrary`（本任务新增）、`k_Fullscreen_vert_spv` / `k_FullscreenCopy_frag_spv`（SPIR-V 头，已在 DeferredPipeline.cpp 包含）。
- Produces: CVar 门控变体演示（默认关闭）。

- [ ] **Step 0: 暴露 GPL 能力到 DeviceCaps**

`IRHIDevice` 接口无 `SupportsGraphicsPipelineLibrary()`（该方法是 `VulkanDevice` 具体类的），演示代码经 `GetCaps()` 查询。在 `DeviceCaps`（`Types.h`，`supportsMeshShaders` 之后）新增：

```cpp
    bool    supportsGraphicsPipelineLibrary = false;  // VK_EXT_graphics_pipeline_library fast-link
```

在 `VulkanDevice::GetCaps()`（`caps.supportsDGC = m_SupportsDGC;` 之后）新增：

```cpp
    caps.supportsGraphicsPipelineLibrary = m_SupportsGPL;
```

- [ ] **Step 1: `DeferredPipeline.h` 新增成员**

在成员区（`m_TransientTestPSO` 附近）新增：

```cpp
    // GPL 变体演示：持有限流器创建的变体 PSO，避免中途销毁
    std::vector<std::unique_ptr<rhi::IRHIPipelineState>> m_GPLVariantPSOs;
```

（`IRHIPipelineState` 通过 `RHI/RHI.h` 已可见；`std::vector` 已在头文件中使用。）

- [ ] **Step 2: `DeferredPipeline.cpp` 新增 CVar 静态量与着色器字节码**

在文件顶部 CVar 区（`cvTransientTest` 之后，`DeferredPipeline.cpp:31` 附近）新增：

```cpp
// CVar: GPL 变体演示开关（0=关闭，1=开启，默认关闭）
// 设为 1 重新编译启用：初始化时生成 N 个仅 blend 状态不同的变体 PSO，
// 经限流器逐帧 fast-link 创建，验证 GPL 四段库缓存与限流协同工作。
static int32_t cvGPLVariantTest = 0;
static int32_t cvGPLVariantCount = 16;  // 变体数量 N

// 变体演示用的全屏三角形 VS + 全屏复制 FS（SPIR-V 头内联字节码）
static rhi::ShaderBytecode g_VariantVS;
static rhi::ShaderBytecode g_VariantFS;
```

- [ ] **Step 3: `DeferredPipeline::Initialize()` 末尾入队变体**

在 `Initialize()` 末尾（`m_Ready = true;` 之前，`DeferredPipeline.cpp:150` 附近）新增：

```cpp
    // ── GPL 变体演示（cvGPLVariantTest=1 且设备支持 GPL 时启用）──
    if (cvGPLVariantTest && device->GetCaps().supportsGraphicsPipelineLibrary) {
        g_VariantVS.stage = rhi::ShaderStage::Vertex;
        g_VariantVS.spirv = k_Fullscreen_vert_spv;
        g_VariantFS.stage = rhi::ShaderStage::Pixel;
        g_VariantFS.spirv = k_FullscreenCopy_frag_spv;

        rhi::PipelineStateDesc base;
        base.bindPoint        = rhi::PipelineBindPoint::Graphics;
        base.vertexShader     = &g_VariantVS;
        base.pixelShader      = &g_VariantFS;
        base.vertexLayout.stride = 0;              // 全屏三角形（SV_VertexID），无顶点输入
        base.colorAttachmentCount = 1;
        base.colorFormats[0]  = rhi::Format::RGBA16_FLOAT;
        base.depthFormat      = rhi::Format::Unknown;  // 无深度
        base.depthTest        = false;
        base.depthWrite       = false;
        base.sampleCount      = 1;

        for (int32_t i = 0; i < cvGPLVariantCount; ++i) {
            // 变体维度：仅 blend 状态不同（改变 fragment-output 段，其余 3 段共享）
            base.colorBlend[0].blendEnable         = true;
            base.colorBlend[0].srcColorBlendFactor =
                static_cast<rhi::BlendFactor>(i % 10);
            base.colorBlend[0].dstColorBlendFactor =
                static_cast<rhi::BlendFactor>((i / 10) % 10);
            device->EnqueuePSOCreate(base);
        }
        HE_CORE_INFO("DeferredPipeline: GPL 变体演示 — 已入队 {} 个变体 PSO",
                     cvGPLVariantCount);
    }
```

- [ ] **Step 4: `DeferredPipeline::NextFrame()` 限流 drain**

在 `NextFrame()` 末尾（`DeferredPipeline.cpp:238` 函数尾 `}` 之前）新增：

```cpp
    // PSO 限流器：每帧最多创建 3 个排队 PSO（变体演示 / 未来材质变体系统）
    static constexpr u32 kMaxPSOCreatesPerFrame = 3;
    if (m_Device->GetPendingPSOCreateCount() > 0) {
        auto created = m_Device->ProcessPSOCreateQueue(kMaxPSOCreatesPerFrame);
        for (auto& pso : created) m_GPLVariantPSOs.push_back(std::move(pso));
        HE_CORE_INFO("DeferredPipeline: 限流创建 {} 个 PSO（待处理 {}）",
                     static_cast<u32>(created.size()),
                     m_Device->GetPendingPSOCreateCount());
    }
```

- [ ] **Step 5: `DeferredPipeline::Shutdown()` 清理**

在 `Shutdown()`（`m_TransientTestPSO.reset();` 之后，`DeferredPipeline.cpp:160` 附近）新增：

```cpp
    m_GPLVariantPSOs.clear();
```

- [ ] **Step 6: 编译 + 运行验证（默认关闭，先验无回归）**

Run: `cmake --build build --config Debug`
Expected: 编译通过。

Run: `build/bin/Debug/04.Deferred.exe`
Expected: `cvGPLVariantTest=0` 时行为与 Task 5 后一致，无变体日志，无回归。

- [ ] **Step 7: 启用验证（临时改 1）**

将 `cvGPLVariantTest` 改为 `1`，重新编译运行：
Expected: 启动日志依次出现
- `DeferredPipeline: GPL 变体演示 — 已入队 16 个变体 PSO`
- 前 6 帧各 `DeferredPipeline: 限流创建 3 个 PSO（待处理 13→…→0）`
- 每条 `Vulkan graphics pipeline created via GPL fast-link`（16 条）
- 四段库命中行为：fragment-output 段创建 16 次、其余 3 段各 1 次（可通过 `GetPendingPSOCreateCount` 逐帧归零 + 无异常确认）
- 零 Validation 错误。验证后把 `cvGPLVariantTest` 改回 `0`。

- [ ] **Step 8: Commit（先向用户确认）**

```bash
git add Engine/Render/Pipeline/DeferredPipeline.h Engine/Render/Pipeline/DeferredPipeline.cpp
git commit -m "Render: 新增 GPL 变体演示 Pass 与限流 drain"
```

---

## 验证清单（全部完成后复验）

1. 全量编译 `cmake --build build --config Debug` 零错误。
2. `cvGPLVariantTest=0`（默认）下 `04.Deferred.exe` 运行正常，传统 VS+FS PSO 走 GPL fast-link、Compute/Mesh 走单片，零 Validation 错误。
3. `cvGPLVariantTest=1` 下 16 个变体经限流器逐帧 fast-link 创建成功。
4. `pipeline_cache.bin` 正常生成/加载，与 Phase 1/3 共存无冲突。
5. 不支持 GPL 的后端回退单片路径（`m_SupportsGPL=false` 时零行为变化）。
