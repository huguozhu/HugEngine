#pragma once

// ============================================================
// TransientResourceAllocator — 瞬态资源分配器（Vulkan 层）
//
// 核心思路：预分配大块 VkDeviceMemory → 帧内 bump 子分配 → 帧末整块回收
// 双缓冲 Heap 池确保 GPU 完成前一帧工作前不覆盖内存
//
// 用法：
//   1. Initialize(device, physical, heapSize)
//   2. 每帧：AllocateImage(createInfo) → VkImage 绑定到当前 Heap
//   3. 帧末：AdvanceFrame() → 切换 Heap，回收上一帧所有子分配
//   4. Shutdown() → 销毁所有堆和缓存
// ============================================================

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "Core/Types.h"

#include <unordered_map>
#include <vector>

namespace he::rhi {

class TransientResourceAllocator {
public:
    // ============================================================
    // 子分配结果：VkImage 绑定到 Heap 的 offset 位置
    // ============================================================
    struct PlacedImage {
        VkImage image   = VK_NULL_HANDLE;
        u64     offset  = 0;   // Heap 内偏移（字节）
        u64     size    = 0;   // 实际占用大小（含对齐）
        u32     heapIdx = 0;   // 所属 Heap 索引
    };

    TransientResourceAllocator()  = default;
    ~TransientResourceAllocator() = default;

    // ============================================================
    // 初始化：分配 kNumHeaps 个 Heap + 创建 per-heap Fence
    // @param heapSize 每个 Heap 的字节大小（默认 128MB）
    // ============================================================
    bool Initialize(VkDevice device, VkPhysicalDevice physical,
                    u64 heapSize = 128 * 1024 * 1024);

    // 销毁所有 Heap、Fence、缓存的 VkImage
    void Shutdown();

    // ============================================================
    // 在当前 Heap 中子分配一个 VkImage
    // 流程：查 ImageCache → 未命中则创建 VkImage → 绑定到 Heap 的 bumpOffset
    // 返回 PlacedImage 中的 VkImage 可用于创建 VkImageView
    // ============================================================
    PlacedImage AllocateImage(const VkImageCreateInfo& info);

    // ============================================================
    // 帧切换：等待并回收下一帧将使用的 Heap
    // 必须在帧开始时调用（在录制任何使用该 Heap 的命令之前）
    // ============================================================
    void AdvanceFrame();

    // ============================================================
    // GPU 完成信号：在提交所有使用当前 Heap 的命令后调用
    // 信号当前 Heap 的 Fence，标记该 Heap 为"GPU 正在使用"
    // ============================================================
    void SignalHeapFence(VkQueue queue);

    // ============================================================
    // 统计信息
    // ============================================================
    u64  GetUsedMemory()  const;
    u64  GetTotalMemory() const;
    u32  GetCurrentHeapIndex() const { return m_CurrentHeap; }
    bool IsInitialized()  const { return m_Device != VK_NULL_HANDLE; }

private:
    // ============================================================
    // VkImage 缓存键：相同参数的 VkImage 可跨帧复用
    // ============================================================
    struct ImageCacheKey {
        VkImageType         imageType;
        VkFormat            format;
        u32                 width;
        u32                 height;
        u32                 depth;
        u32                 mipLevels;
        u32                 arrayLayers;
        VkSampleCountFlagBits samples;
        VkImageUsageFlags   usage;
        VkImageCreateFlags  flags;

        bool operator==(const ImageCacheKey& other) const;
    };
    struct ImageCacheKeyHash {
        usize operator()(const ImageCacheKey& k) const;
    };

    // ============================================================
    // Heap 结构：一块 VkDeviceMemory + Bump Allocator 状态
    // ============================================================
    static constexpr u32 kNumHeaps = 2;  // 双缓冲

    struct Heap {
        VkDeviceMemory memory      = VK_NULL_HANDLE;
        u64            size        = 0;
        u64            bumpOffset  = 0;    // 当前分配游标
        VkFence        fence       = VK_NULL_HANDLE; // GPU 完成此 Heap 的信号
        bool           fenceSignaled = false; // 当前是否已信号（提交后为 true）
    };

    // ============================================================
    // 内部辅助方法
    // ============================================================
    u32  FindMemoryType(u32 typeFilter, VkMemoryPropertyFlags props) const;
    void DestroyCachedImages();  // 销毁所有缓存的 VkImage 对象

    VkDevice         m_Device       = VK_NULL_HANDLE;
    VkPhysicalDevice m_Physical     = VK_NULL_HANDLE;
    u64              m_HeapSize     = 0;

    Heap             m_Heaps[kNumHeaps];
    u32              m_CurrentHeap  = 0;
    u64              m_FrameIndex   = 0;

    // VkImage 缓存：key → 可复用的 VkImage 列表
    std::unordered_map<ImageCacheKey, std::vector<VkImage>, ImageCacheKeyHash> m_ImageCache;

    // 统计
    u64 m_PeakUsedMemory  = 0;
    u32 m_CacheHitCount   = 0;
    u32 m_CacheMissCount  = 0;
};

} // namespace he::rhi
