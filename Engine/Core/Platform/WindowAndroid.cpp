// ============================================================
// WindowAndroid.cpp —— Android 窗口实现（ANativeWindow 来自 SurfaceView）
//
// 与 Windows 的最大差异：**窗口不是引擎创建的**。Java 侧的 SurfaceView 在
// surfaceCreated 时把 Surface 交给 JNI，JNI 取出 ANativeWindow* 再调用
// AttachNativeWindow()。尺寸/句柄查询都返回这里保存的值。
//
// 【生命周期要点】
//   · 旋转/分屏：只更新尺寸并重设窗口缓冲几何；
//   · 切后台再回来：Surface 可能重建，Attach 会替换句柄；
//   · Detach：只清空指针（ANativeWindow 由 Java 侧 Surface 持有，不能在此释放）。
// ============================================================

#include "Core/Platform.h"

#include "Platform/IWindow.h"
#include "Platform/WindowAndroid.h"

#if HE_PLATFORM_ANDROID

#include "Core/Log.h"

#include <atomic>
#include <mutex>

namespace he {
namespace {

std::mutex        g_WindowMutex;
ANativeWindow*    g_NativeWindow = nullptr;
std::atomic<bool> g_ShouldClose{ false };
std::atomic<u32>  g_Width{ 0 };
std::atomic<u32>  g_Height{ 0 };
std::atomic<f32>  g_ContentScale{ 1.0f };

/// 让系统按新尺寸重新分配窗口缓冲（旋转/分屏必须做，否则交换链与窗口缓冲尺寸不一致）
void ApplyBufferGeometry(ANativeWindow* window, u32 width, u32 height) {
    if (!window || width == 0 || height == 0) return;
    const int32_t rc = ANativeWindow_setBuffersGeometry(window,
                                                        static_cast<int32_t>(width),
                                                        static_cast<int32_t>(height),
                                                        WINDOW_FORMAT_RGBA_8888);
    if (rc != 0) {
        HE_CORE_WARN("[Android] 设置窗口缓冲几何失败: {}x{} rc={}", width, height, rc);
    }
}

} // namespace

void AttachNativeWindow(ANativeWindow* window, u32 width, u32 height) {
    std::lock_guard<std::mutex> lock(g_WindowMutex);

    if (window) {
        if (g_NativeWindow && g_NativeWindow != window) {
            HE_CORE_INFO("[Android] 原生窗口句柄已更换");
        }
        g_NativeWindow = window;
        ApplyBufferGeometry(window, width, height);
    } else if (g_NativeWindow) {
        // nullptr = 只更新尺寸（resize 路径）
        ApplyBufferGeometry(g_NativeWindow, width, height);
    }

    g_Width.store(width);
    g_Height.store(height);
    HE_CORE_INFO("[Android] 原生窗口已挂接: {}x{}", width, height);
}

void DetachNativeWindow() {
    std::lock_guard<std::mutex> lock(g_WindowMutex);
    g_NativeWindow = nullptr;
    HE_CORE_INFO("[Android] 原生窗口已断开");
}

void SetContentScale(f32 scale) {
    if (scale > 0.0f) {
        g_ContentScale.store(scale);
        HE_CORE_INFO("[Android] 内容缩放设置为 {}", scale);
    }
}

class WindowAndroid final : public IWindow {
public:
    explicit WindowAndroid(const WindowDesc& desc) : m_Desc(desc) {
        g_Width.store(desc.width);
        g_Height.store(desc.height);
        HE_CORE_INFO("[Android] WindowAndroid 已创建（等待 SurfaceView 提供 surface）");
    }

    bool ShouldClose() const override { return g_ShouldClose.load(); }
    void RequestClose() override { g_ShouldClose.store(true); }

    // 事件由 Java 侧驱动：native 侧只做每帧一次的轻量泵（不阻塞）
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
    void* GetDisplayHandle() const override { return nullptr; }
    f32   GetContentScale() const override { return g_ContentScale.load(); }

    // Android 换 surface 的代价是 Activity/SurfaceView 重建，不做原地 Recreate
    bool Recreate() override {
        HE_CORE_WARN("[Android] WindowAndroid::Recreate 不支持（surface 由 SurfaceView 管理）");
        return false;
    }

    void SetResizeCallback(ResizeCallback cb) override { m_OnResize = std::move(cb); }

    void SetCursorVisible(bool) override {}   // 触摸设备无光标
    void SetTitle(const char*) override {}    // 标题由 Activity 决定

private:
    WindowDesc     m_Desc{};
    ResizeCallback m_OnResize;
};

std::unique_ptr<IWindow> CreatePlatformWindow(const WindowDesc& desc) {
    return std::make_unique<WindowAndroid>(desc);
}

} // namespace he

#endif // HE_PLATFORM_ANDROID
