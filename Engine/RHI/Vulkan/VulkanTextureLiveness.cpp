#include "VulkanTextureLiveness.h"

// ============================================================
// 纹理存活登记表实现
//
// 用 unordered_set 存指针值：纹理对象的地址在生命周期内唯一，
// 销毁后从集合移除，因此集合命中 == "这个指针现在仍然指向一个活着的纹理"。
// 多线程安全：纹理可能在加载线程创建、在渲染线程销毁，故加互斥锁。
// ============================================================

#include <mutex>
#include <unordered_set>

namespace he::rhi {

namespace {
    std::mutex                                     g_Mutex;
    std::unordered_set<const void*>                g_AliveTextures;
}

void MarkTextureAlive(const void* texture) {
    if (!texture) return;
    std::lock_guard<std::mutex> lock(g_Mutex);
    g_AliveTextures.insert(texture);
}

void MarkTextureDead(const void* texture) {
    if (!texture) return;
    std::lock_guard<std::mutex> lock(g_Mutex);
    g_AliveTextures.erase(texture);
}

bool IsTextureAlive(const void* texture) {
    if (!texture) return false;
    std::lock_guard<std::mutex> lock(g_Mutex);
    return g_AliveTextures.find(texture) != g_AliveTextures.end();
}

unsigned int GetAliveTextureCount() {
    std::lock_guard<std::mutex> lock(g_Mutex);
    return static_cast<unsigned int>(g_AliveTextures.size());
}

} // namespace he::rhi
