// ============================================================
// TransientResourceAllocator.cpp — 瞬态资源分配器实现
//
// 双缓冲 Heap 池 + Bump Allocator + VkImage 对象缓存
// ============================================================

#include "TransientResourceAllocator.h"
#include "Core/Log.h"
#include "Core/Assert.h"

#include <cstring>

namespace he::rhi {

// ============================================================
// ImageCacheKey 哈希实现
// ============================================================

bool TransientResourceAllocator::ImageCacheKey::operator==(const ImageCacheKey& other) const {
    return imageType   == other.imageType
        && format       == other.format
        && width        == other.width
        && height       == other.height
        && depth        == other.depth
        && mipLevels    == other.mipLevels
        && arrayLayers  == other.arrayLayers
        && samples      == other.samples
        && usage        == other.usage
        && flags        == other.flags;
}

usize TransientResourceAllocator::ImageCacheKeyHash::operator()(const ImageCacheKey& k) const {
    // FNV-1a 64-bit 哈希，折叠为 usize
    u64 h = 0xcbf29ce484222325ULL;
    auto hashU32 = [&](u32 v) { h ^= (v & 0xFF); h ^= ((v >> 8) & 0xFF); h ^= ((v >> 16) & 0xFF); h ^= ((v >> 24) & 0xFF); h *= 0x100000001b3ULL; };
    hashU32(static_cast<u32>(k.imageType));
    hashU32(static_cast<u32>(k.format));
    hashU32(k.width);
    hashU32(k.height);
    hashU32(k.depth);
    hashU32(k.mipLevels);
    hashU32(k.arrayLayers);
    hashU32(static_cast<u32>(k.samples));
    hashU32(static_cast<u32>(k.usage));
    hashU32(static_cast<u32>(k.flags));
    return static_cast<usize>(h);
}

// ============================================================
// 初始化
// ============================================================

bool TransientResourceAllocator::Initialize(VkDevice device, VkPhysicalDevice physical, u64 heapSize) {
    m_Device   = device;
    m_Physical = physical;
    m_HeapSize = heapSize;

    for (u32 i = 0; i < kNumHeaps; ++i) {
        Heap& heap = m_Heaps[i];
        heap.size       = heapSize;
        heap.bumpOffset = 0;

        // 分配 VkDeviceMemory
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize  = heapSize;
        allocInfo.memoryTypeIndex = FindMemoryType(~0u,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        VkResult result = vkAllocateMemory(device, &allocInfo, nullptr, &heap.memory);
        if (result != VK_SUCCESS) {
            HE_CORE_ERROR("TransientAllocator: 无法分配 Heap{} ({:.0f}MB), result={}",
                i, double(heapSize) / (1024.0 * 1024.0), int(result));
            // 回退：清空已分配的 Heap
            for (u32 j = 0; j < i; ++j) {
                if (m_Heaps[j].memory) vkFreeMemory(device, m_Heaps[j].memory, nullptr);
                if (m_Heaps[j].fence)  vkDestroyFence(device, m_Heaps[j].fence, nullptr);
            }
            m_Device = VK_NULL_HANDLE;
            return false;
        }

    }

    HE_CORE_INFO("TransientAllocator: 初始化完成 — {} Heap × {:.0f}MB = {:.0f}MB",
        kNumHeaps,
        double(heapSize) / (1024.0 * 1024.0),
        double(heapSize * kNumHeaps) / (1024.0 * 1024.0));
    return true;
}

void TransientResourceAllocator::Shutdown() {
    if (!m_Device) return;

    // 等待 GPU 完成所有工作（安全销毁）
    vkDeviceWaitIdle(m_Device);

    // 销毁所有缓存的 VkImage
    DestroyCachedImages();

    // 销毁 Heap 和 Fence
    for (u32 i = 0; i < kNumHeaps; ++i) {
        Heap& heap = m_Heaps[i];
        if (heap.memory) { vkFreeMemory(m_Device, heap.memory, nullptr); heap.memory = VK_NULL_HANDLE; }
        if (heap.fence)  { vkDestroyFence(m_Device, heap.fence, nullptr);   heap.fence  = VK_NULL_HANDLE; }
        heap.size       = 0;
        heap.bumpOffset = 0;
    }

    m_Device   = VK_NULL_HANDLE;
    m_Physical = VK_NULL_HANDLE;
    HE_CORE_INFO("TransientAllocator: 已销毁 (峰值使用 {:.1f}MB / {:.1f}MB, 缓存命中率 {:.0f}%)",
        double(m_PeakUsedMemory) / (1024.0 * 1024.0),
        double(m_HeapSize) / (1024.0 * 1024.0),
        m_CacheHitCount + m_CacheMissCount > 0
            ? 100.0 * m_CacheHitCount / (m_CacheHitCount + m_CacheMissCount)
            : 0.0);
}

// ============================================================
// AllocateImage — 核心子分配逻辑
// ============================================================

TransientResourceAllocator::PlacedImage
TransientResourceAllocator::AllocateImage(const VkImageCreateInfo& info) {
    Heap& heap = m_Heaps[m_CurrentHeap];

    // 1. 构建缓存键
    ImageCacheKey key;
    key.imageType   = info.imageType;
    key.format      = info.format;
    key.width       = info.extent.width;
    key.height      = info.extent.height;
    key.depth       = info.extent.depth;
    key.mipLevels   = info.mipLevels;
    key.arrayLayers = info.arrayLayers;
    key.samples     = info.samples;
    key.usage       = info.usage;
    key.flags       = info.flags;

    // 2. 查询 VkImage 缓存：尝试复用已有 VkImage 对象
    VkImage image = VK_NULL_HANDLE;
    auto cacheIt = m_ImageCache.find(key);
    if (cacheIt != m_ImageCache.end() && !cacheIt->second.empty()) {
        image = cacheIt->second.back();
        cacheIt->second.pop_back();
        m_CacheHitCount++;
    } else {
        // 缓存未命中：创建新 VkImage
        VkResult result = vkCreateImage(m_Device, &info, nullptr, &image);
        if (result != VK_SUCCESS) {
            HE_CORE_ERROR("TransientAllocator: vkCreateImage 失败 (result={})", int(result));
            return {};
        }
        m_CacheMissCount++;
    }

    // 3. 查询该 Image 的内存需求（对齐 + 大小）
    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(m_Device, image, &memReqs);

    // 4. Bump 分配：对齐 offset
    u64 alignedOffset = (heap.bumpOffset + memReqs.alignment - 1) & ~(memReqs.alignment - 1);

    // 检查 Heap 空间是否足够
    if (alignedOffset + memReqs.size > heap.size) {
        HE_CORE_ERROR("TransientAllocator: Heap{} 内存不足! "
            "需要 {:.1f}MB, 已用 {:.1f}MB, 总量 {:.1f}MB。请增大 heapSize",
            m_CurrentHeap,
            double(memReqs.size) / (1024.0 * 1024.0),
            double(alignedOffset) / (1024.0 * 1024.0),
            double(heap.size) / (1024.0 * 1024.0));
        // 回退：将 Image 放回缓存，返回空结果
        m_ImageCache[key].push_back(image);
        return {};
    }

    // 5. 绑定 VkImage 到 Heap 的 offset 位置
    VkBindImageMemoryInfo bindInfo{};
    bindInfo.sType        = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;
    bindInfo.image        = image;
    bindInfo.memory       = heap.memory;
    bindInfo.memoryOffset = alignedOffset;

    VkResult result = vkBindImageMemory2(m_Device, 1, &bindInfo);
    if (result != VK_SUCCESS) {
        HE_CORE_ERROR("TransientAllocator: vkBindImageMemory2 失败 (result={})", int(result));
        m_ImageCache[key].push_back(image);
        return {};
    }

    // 6. 更新 bump 指针
    heap.bumpOffset = alignedOffset + memReqs.size;

    // 统计峰值
    if (heap.bumpOffset > m_PeakUsedMemory)
        m_PeakUsedMemory = heap.bumpOffset;

    PlacedImage placed;
    placed.image   = image;
    placed.offset  = alignedOffset;
    placed.size    = memReqs.size;
    placed.heapIdx = m_CurrentHeap;
    return placed;
}

// ============================================================
// AdvanceFrame — 帧切换
//
// 双缓冲 Heap 安全性说明：
//   Frame N 使用 Heap A → Frame N+1 使用 Heap B → Frame N+2 使用 Heap A
//   由于 SwapChain Present 本身提供至少 2 帧的 GPU 流水线间隔
//   （VK_PRESENT_MODE_FIFO 默认最少 2 个 swapchain image），
//   Heap A 在 Frame N+2 被重用时 GPU 已确定完成 Frame N 的所有工作。
//   无需显式 fence 等待。
// ============================================================

void TransientResourceAllocator::AdvanceFrame() {
    if (!m_Device) return;

    // 切换到下一个 Heap 并重置其 bump 指针
    u32 nextHeap = (m_CurrentHeap + 1) % kNumHeaps;
    m_Heaps[nextHeap].bumpOffset = 0;

    m_CurrentHeap = nextHeap;
    m_FrameIndex++;

    HE_CORE_INFO("TransientAllocator: Frame {} — 切换到 Heap{} (峰值使用 {:.1f}MB / {:.0f}MB)",
        m_FrameIndex, m_CurrentHeap,
        double(m_PeakUsedMemory) / (1024.0 * 1024.0),
        double(m_HeapSize) / (1024.0 * 1024.0));
}

// ============================================================
// SignalHeapFence — GPU 完成信号（预留接口，Phase 3+ 优化用）
// 当前帧切换不依赖 fence，保留此接口供未来 per-heap fence 优化
// ============================================================

void TransientResourceAllocator::SignalHeapFence(VkQueue /*queue*/) {
    // Phase 2 基本版：无需显式 fence，双缓冲 + SwapChain 天然安全
    // Phase 3 优化版：在此实现 per-heap fence 以减少延迟
}

// ============================================================
// 统计
// ============================================================

u64 TransientResourceAllocator::GetUsedMemory() const {
    return m_Heaps[m_CurrentHeap].bumpOffset;
}

u64 TransientResourceAllocator::GetTotalMemory() const {
    return m_HeapSize;
}

// ============================================================
// 内部辅助
// ============================================================

u32 TransientResourceAllocator::FindMemoryType(u32 typeFilter, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_Physical, &memProps);

    for (u32 i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    HE_ASSERT(false, "TransientAllocator: 未找到合适的设备本地内存类型");
    return 0;
}

void TransientResourceAllocator::DestroyCachedImages() {
    for (auto& [key, images] : m_ImageCache) {
        for (VkImage img : images) {
            vkDestroyImage(m_Device, img, nullptr);
        }
    }
    m_ImageCache.clear();
}

} // namespace he::rhi
