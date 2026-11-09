// ============================================================
// WindowOHOS.cpp —— 鸿蒙窗口实现（OHNativeWindow 来自 XComponent）
//
// 与 Windows 的最大差异：**窗口不是引擎创建的**。ArkTS 侧的 XComponent 在
// surface 就绪时把 surfaceId 交给 NativeEntry，后者换成 OHNativeWindow* 再调用
// AttachNativeWindow()。此前 IWindow 的尺寸/句柄查询都返回占位值。
//
// 【生命周期要点】
//   · 旋转/分屏：只更新尺寸并重设缓冲几何（不重建窗口句柄）；
//   · 切后台再回来：XComponent 可能给出**新的** surface，Attach 会销毁旧句柄；
//   · Detach：销毁原生窗口句柄，避免每次 surface 重建泄漏一个 OHNativeWindow。
// ============================================================

#include "Core/Platform.h"   // HE_PLATFORM_*

#include "Platform/IWindow.h"
#include "Platform/WindowOHOS.h"

#if HE_PLATFORM_OHOS

#include "Core/Log.h"
#include "Core/Assert.h"

#include <native_window/external_window.h>

#include <atomic>
#include <mutex>

namespace he {
namespace {

// 由 NativeEntry 在 surface 就绪时写入；销毁/切后台时清空
std::mutex        g_WindowMutex;
OHNativeWindow*   g_NativeWindow = nullptr;
std::atomic<bool> g_ShouldClose{ false };
std::atomic<u32>  g_Width{ 0 };
std::atomic<u32>  g_Height{ 0 };
std::atomic<f32>  g_ContentScale{ 1.0f };   // 屏幕密度（ArkTS 侧设置）

/// 让系统按新尺寸重新分配窗口缓冲（旋转/分屏必须做，否则 Vulkan 交换链与窗口缓冲尺寸不一致）
void ApplyBufferGeometry(OHNativeWindow* window, u32 width, u32 height) {
    if (!window || width == 0 || height == 0) return;
    const int32_t rc = OH_NativeWindow_NativeWindowHandleOpt(
        window, SET_BUFFER_GEOMETRY, static_cast<int>(width), static_cast<int>(height));
    if (rc != 0) {
        HE_CORE_WARN("[OHOS] 设置窗口缓冲几何失败: {}x{} rc={}", width, height, rc);
    }
}

} // namespace

void AttachNativeWindow(OHNativeWindow* window, u32 width, u32 height) {
    std::lock_guard<std::mutex> lock(g_WindowMutex);

    if (window) {
        // 换了新的原生窗口句柄（切后台回来 / XComponent 重建）：先释放旧的
        if (g_NativeWindow && g_NativeWindow != window) {
            OH_NativeWindow_DestroyNativeWindow(g_NativeWindow);
            HE_CORE_INFO("[OHOS] 原生窗口句柄已更换，旧句柄已销毁");
        }
        g_NativeWindow = window;
        ApplyBufferGeometry(window, width, height);
    } else if (g_NativeWindow) {
        // nullptr = 只更新尺寸（resize 路径），不替换句柄，但要同步缓冲几何
        ApplyBufferGeometry(g_NativeWindow, width, height);
    }

    g_Width.store(width);
    g_Height.store(height);
    HE_CORE_INFO("[OHOS] 原生窗口已挂接: {}x{}（{:p}）", width, height, static_cast<void*>(g_NativeWindow));
}

void SetContentScale(f32 scale) {
    if (scale > 0.0f) {
        g_ContentScale.store(scale);
        HE_CORE_INFO("[OHOS] 内容缩放设置为 {}", scale);
    }
}

void DetachNativeWindow() {
    std::lock_guard<std::mutex> lock(g_WindowMutex);
    if (g_NativeWindow) {
        OH_NativeWindow_DestroyNativeWindow(g_NativeWindow);
        g_NativeWindow = nullptr;
    }
    HE_CORE_INFO("[OHOS] 原生窗口已断开");
}

class WindowOHOS final : public IWindow {
public:
    explicit WindowOHOS(const WindowDesc& desc) : m_Desc(desc) {
        g_Width.store(desc.width);
        g_Height.store(desc.height);
        HE_CORE_INFO("[OHOS] WindowOHOS 已创建（等待 XComponent 提供 surface）");
    }

    bool ShouldClose() const override { return g_ShouldClose.load(); }
    void RequestClose() override { g_ShouldClose.store(true); }

    // 事件由 ArkTS 侧驱动：native 侧只做每帧一次的轻量泵（不阻塞）
    void PollEvents() override {}

    u32 GetWidth() const override { return g_Width.load(); }
    u32 GetHeight() const override { return g_Height.load(); }
    float2 GetSize() const override {
        return float2(static_cast<float>(GetWidth()), static_cast<float>(GetHeight()));
    }
    float GetAspectRatio() const override {
        const u32 w = GetWidth(), h = GetHeight();
        return (w && h) ? static_cast<float>(w) / static_cast<float>(h) : 1.0f;
    }

    void* GetNativeWindow() const override {
        std::lock_guard<std::mutex> lock(g_WindowMutex);
        return static_cast<void*>(g_NativeWindow);
    }
    void* GetDisplayHandle() const override { return nullptr; }   // 鸿蒙不需要 HINSTANCE
    f32 GetContentScale() const override { return g_ContentScale.load(); }

    // 鸿蒙换 window 的代价是整块 XComponent 重建，不做原地 Recreate：
    // 新 surface 到来时 AttachNativeWindow 会替换句柄，渲染线程按需重建交换链。
    bool Recreate() override {
        HE_CORE_WARN("[OHOS] WindowOHOS::Recreate 不支持（surface 由 XComponent 生命周期管理）");
        return false;
    }

    void SetResizeCallback(ResizeCallback cb) override { m_OnResize = std::move(cb); }

    void SetCursorVisible(bool) override {}      // 触摸设备无光标
    void SetTitle(const char*) override {}       // 标题由 ArkTS 侧页面决定

private:
    WindowDesc     m_Desc{};
    ResizeCallback m_OnResize;
};

std::unique_ptr<IWindow> CreatePlatformWindow(const WindowDesc& desc) {
    return std::make_unique<WindowOHOS>(desc);
}

} // namespace he

#endif // HE_PLATFORM_OHOS
