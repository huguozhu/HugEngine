// ============================================================

#include "Core/Platform.h"   // HE_PLATFORM_*：必须在使用 #if 之前可见
// InputOHOS.cpp —— 鸿蒙输入采样（触摸事件队列 → InputState）

//

// 事件由 XComponent 的 napi 回调经 he::ohos::PushTouchEvent 入队（可能在 ArkTS 线程），

// 渲染线程每帧调用 PollInput 时取出并换算：

//   · 单指按下/拖动 → 作为"右键拖动"暴露给 InputState（mouseDown[1] / mouseDelta），

//     这样桌面版 CameraInput 无需改动即可在手机上旋转相机；

//   · 坐标是归一化 [0,1]，按窗口尺寸换算成像素位移，保证灵敏度与分辨率无关。

// ============================================================


#include "Platform/IInput.h"

#if HE_PLATFORM_OHOS

#include "Platform/IWindow.h"
#include "Platform/InputOHOS.h"

#include "Core/Log.h"

#include <deque>
#include <mutex>

namespace he {
namespace {

struct RawTouch {
    u32        id    = 0;
    ohos::TouchPhase phase = ohos::TouchPhase::Began;
    float      x     = 0.0f;   // 归一化
    float      y     = 0.0f;
};

std::mutex        g_QueueMutex;
std::deque<RawTouch> g_Queue;

struct OhosInputState {
    InputState state;
    bool       fingerDown   = false;
    float      lastX = 0.0f, lastY = 0.0f;   // 上一帧的归一化位置
};

} // namespace

namespace ohos {

void PushTouchEvent(u32 id, TouchPhase phase, float xNorm, float yNorm) {
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        HE_CORE_INFO("[OHOS] 首次收到触摸: id={} phase={} ({:.3f}, {:.3f})", id, static_cast<int>(phase), xNorm, yNorm);
    }
    std::lock_guard<std::mutex> lock(g_QueueMutex);
    g_Queue.push_back(RawTouch{ id, phase, xNorm, yNorm });
}

void PushKeyEvent(int, bool) {
    // P3：需要物理按键映射时再实现
}

} // namespace ohos

InputState& PollInput(IWindow* window) {
    static OhosInputState s;
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
        // 只跟踪第一个手指（多指手势在 P3 里再加：双指缩放/平移）
        if (e.id != 0) continue;

        const float px = e.x * w;
        const float py = e.y * h;

        switch (e.phase) {
            case ohos::TouchPhase::Began:
                s.fingerDown = true;
                s.lastX = px; s.lastY = py;
                st.mouseDown[1]    = true;
                st.mousePressed[1] = true;
                break;
            case ohos::TouchPhase::Moved:
                if (s.fingerDown) {
                    st.mouseDelta.x += px - s.lastX;
                    st.mouseDelta.y += py - s.lastY;
                    s.lastX = px; s.lastY = py;
                }
                break;
            case ohos::TouchPhase::Ended:
            case ohos::TouchPhase::Cancelled:
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

#endif // HE_PLATFORM_OHOS
