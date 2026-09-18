// ============================================================
// RHI/TextureLayoutTracker.cpp — 纹理当前布局追踪实现
//
// 用 unordered_map 存「纹理视图句柄 → 资源状态」，加互斥锁（纹理可能在
// 加载线程创建、渲染线程销毁/使用）。查找/写入都是 O(1)，且只在 barrier
// 与纹理销毁时发生，不在热路径上。
// ============================================================

#include "RHI/TextureLayoutTracker.h"

#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace he::rhi {

namespace {
    std::mutex                              g_Mutex;
    std::unordered_map<void*, ResourceState> g_Layouts;
}

void TrackTextureLayout(void* imageView, ResourceState state) {
    if (!imageView) return;
    std::lock_guard<std::mutex> lock(g_Mutex);
    g_Layouts[imageView] = state;
}

bool QueryTrackedTextureLayout(void* imageView, ResourceState& outState) {
    if (!imageView) return false;
    std::lock_guard<std::mutex> lock(g_Mutex);
    auto it = g_Layouts.find(imageView);
    if (it == g_Layouts.end()) return false;
    outState = it->second;
    return true;
}

void ForgetTrackedTextureLayout(void* imageView) {
    if (!imageView) return;
    std::lock_guard<std::mutex> lock(g_Mutex);
    g_Layouts.erase(imageView);
}

// ---- 视图 → 底层图像 登记 ----

namespace {
    struct ViewImageInfo {
        void* image       = nullptr;
        u32   mipLevels   = 1;
        u32   arrayLayers = 1;
        u32   format = 0;   // u32(Format)：render pass 边界回写真实布局时用
};
    std::unordered_map<void*, ViewImageInfo> g_ViewImages;

    // 「该图像是否被写入过」与「是否已就该图像告过警」——均按**图像**记：
    // 立方体贴图是逐面视图写入、却以整张 cube 视图被采样，按视图记会误判为"从未写入"。
    std::unordered_set<void*> g_WrittenImages;
    std::unordered_set<void*> g_WarnedImages;
    // 被采样、但当时尚未写入的**累计次数**（见下方 kUnwrittenWarnThreshold 的说明）
    std::unordered_map<void*, u32> g_UnwrittenSamples;

    // 告警阈值：连续累计这么多次「被采样时还没写过」才判定为可疑。
    // 直接用「第一次采样时没写过」就报警会产生大量假阳性——同一帧内先采样后写入
    // （前一帧产物、按注册顺序排在后方的 pass）是正常现象，首帧尤其密集。
    // 这类假阳性只出现一两次，之后图像就被登记为已写入、计数清零；而真正的写漏会被
    // 每一帧都采样到，很快越过阈值。30 次约等于 30 帧（1 秒左右）。
    constexpr u32 kUnwrittenWarnThreshold = 30;

    /// 调用方须已持锁
    void* ImageOfViewLocked(void* imageView) {
        auto it = g_ViewImages.find(imageView);
        return (it == g_ViewImages.end()) ? nullptr : it->second.image;
    }
}

void TrackViewImage(void* imageView, void* image, u32 mipLevels, u32 arrayLayers, u32 format) {
    if (!imageView) return;
    std::lock_guard<std::mutex> lock(g_Mutex);
    g_ViewImages[imageView] = ViewImageInfo{ image, mipLevels ? mipLevels : 1,
                                            arrayLayers ? arrayLayers : 1, format };
}

bool QueryViewFormat(void* imageView, u32& outFormat) {
    if (!imageView) return false;
    std::lock_guard<std::mutex> lock(g_Mutex);
    auto it = g_ViewImages.find(imageView);
    if (it == g_ViewImages.end()) return false;
    outFormat = it->second.format;
    return true;
}

bool QueryViewImage(void* imageView, void*& outImage, u32& outMipLevels, u32& outArrayLayers) {
    if (!imageView) return false;
    std::lock_guard<std::mutex> lock(g_Mutex);
    auto it = g_ViewImages.find(imageView);
    if (it == g_ViewImages.end()) return false;
    outImage       = it->second.image;
    outMipLevels   = it->second.mipLevels;
    outArrayLayers = it->second.arrayLayers;
    return outImage != nullptr;
}

void ForgetViewImage(void* imageView) {
    if (!imageView) return;
    std::lock_guard<std::mutex> lock(g_Mutex);
    void* image = ImageOfViewLocked(imageView);
    g_ViewImages.erase(imageView);
    if (!image) return;
    // 同一图像可能还有别的视图（如立方体贴图的逐面视图）；全部注销后才清理派生状态，
    // 否则视图句柄被复用时会误判成"已写入"
    for (auto& kv : g_ViewImages) {
        if (kv.second.image == image) return;
    }
    g_WrittenImages.erase(image);
    g_WarnedImages.erase(image);
    g_UnwrittenSamples.erase(image);
}

// ---- 图像「已写入」追踪 ----

void MarkViewWritten(void* imageView) {
    if (!imageView) return;
    std::lock_guard<std::mutex> lock(g_Mutex);
    void* image = ImageOfViewLocked(imageView);
    if (image) {
        g_WrittenImages.insert(image);
        g_UnwrittenSamples.erase(image);   // 写过了：撤销可疑计数，避免历史累计误伤
    }
}

bool IsViewWritten(void* imageView, bool* outFirstWarn) {
    if (outFirstWarn) *outFirstWarn = false;
    if (!imageView) return true;                 // 无视图：不判定
    std::lock_guard<std::mutex> lock(g_Mutex);
    void* image = ImageOfViewLocked(imageView);
    if (!image) return true;                     // 未登记的视图：保守放行，不制造假阳性
    if (g_WrittenImages.find(image) != g_WrittenImages.end()) {
        g_UnwrittenSamples.erase(image);
        return true;
    }
    if (g_WarnedImages.find(image) != g_WarnedImages.end()) return true;   // 已就这张图告过警
    // 还没写过：累计可疑次数，越过阈值才判定（滤掉首帧/跨 pass 顺序造成的假阳性）
    u32& unwritten = g_UnwrittenSamples[image];
    if (++unwritten < kUnwrittenWarnThreshold) return true;
    g_WarnedImages.insert(image);
    if (outFirstWarn) *outFirstWarn = true;
    return false;
}

} // namespace he::rhi
