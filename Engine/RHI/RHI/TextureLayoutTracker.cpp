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

} // namespace he::rhi
