// ============================================================
// InputAndroid.cpp —— Android 输入采样（触摸事件队列 → InputState）
//
// 与鸿蒙 InputOHOS 完全同一套语义：
//   Java 的 MotionEvent 归一化成 [0,1] 后经 he::PushTouchPoint 入队（UI 线程），
//   渲染线程每帧 PollInput 取出并换算成"右键拖动"（mouseDown[1] / mouseDelta），
//   让桌面版 CameraInput 直接可用。
// ============================================================

#include "Core/Platform.h"

#include "Platform/IInput.h"

#if HE_PLATFORM_ANDROID

#include "Platform/IWindow.h"
#include "Platform/InputAndroid.h"

#include "Core/Log.h"

#include <deque>
#include <mutex>

namespace he {
namespace {

struct RawTouch {
    u32        id    = 0;
    TouchPhase phase = TouchPhase::Began;
    float      x     = 0.0f;   // 归一化
    float      y     = 0.0f;
};

std::mutex           g_QueueMutex;
std::deque<RawTouch> g_Queue;

struct MobileInputState {
    InputState state;
    bool       fingerDown = false;
    float      lastX = 0.0f, lastY = 0.0f;
};

} // namespace

// 平台无关入口：Android JNI 桥接层调用它
void PushTouchPoint(u32 id, TouchPhase phase, float xNorm, float yNorm) {
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        HE_CORE_INFO("[Android] 首次收到触摸: id={} phase={} ({:.3f}, {:.3f})",
                     id, static_cast<int>(phase), xNorm, yNorm);
    }
    std::lock_guard<std::mutex> lock(g_QueueMutex);
    g_Queue.push_back(RawTouch{ id, phase, xNorm, yNorm });
}

namespace android {

void PushKeyEvent(int, bool) {
    // 需要物理按键映射时再实现
}

} // namespace android

InputState& PollInput(IWindow* window) {
    static MobileInputState s;
    InputState& st = s.state;
    for (u32 i = 0; i < kKeyCount; ++i) st.keysPressed[i] = false;
    for (u32 i = 0; i < 3; ++i) st.mousePressed[i] = false;

    const float w = window ? static_cast<float>(window->GetWidth())  : 1.0f;
    const float h = window ? static_cast<float>(window->GetHeight()) : 1.0f;

    std::deque<RawTouch> events;
    {
        std::lock_guard<std::mutex> lock(g_QueueMutex);
        events.swap(g_Queue);
    }

    st.mouseDelta = float2(0.0f, 0.0f);
    for (const RawTouch& e : events) {
        if (e.id != 0) continue;   // 只跟踪第一根手指

        const float px = e.x * w;
        const float py = e.y * h;

        switch (e.phase) {
            case TouchPhase::Began:
                s.fingerDown = true;
                s.lastX = px; s.lastY = py;
                st.mouseDown[1]    = true;
                st.mousePressed[1] = true;
                break;
            case TouchPhase::Moved:
                if (s.fingerDown) {
                    st.mouseDelta.x += px - s.lastX;
                    st.mouseDelta.y += py - s.lastY;
                    s.lastX = px; s.lastY = py;
                }
                break;
            case TouchPhase::Ended:
            case TouchPhase::Cancelled:
                s.fingerDown   = false;
                st.mouseDown[1] = false;
                break;
        }
        st.mousePos = float2(px, py);
    }
    st.touchCount = s.fingerDown ? 1u : 0u;
    return st;
}

} // namespace he

#endif // HE_PLATFORM_ANDROID
