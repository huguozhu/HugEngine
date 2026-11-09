#pragma once

#include "Core/Platform.h"   // HE_PLATFORM_*：必须在使用 #if 之前可见

// ============================================================
// InputOHOS.h —— 鸿蒙输入注入接口（给 NativeEntry / ArkTS 回调调用）
//
// 鸿蒙没有 GLFW：触摸/按键事件由 XComponent 的 napi 回调传入，先入队，
// 再由 PollInput（InputOHOS.cpp）换算成平台无关的 InputState。
// 触摸映射规则（与桌面鼠标语义对齐，让 CameraInput 直接可用）：
//   · 单指按下/拖动  → 等价"按住右键拖动"（相机旋转）
//   · 单指抬起        → 等价"松开右键"
// ============================================================

#include "Core/Types.h"

#if HE_PLATFORM_OHOS

namespace he::ohos {

enum class TouchPhase { Began = 0, Moved = 1, Ended = 2, Cancelled = 3 };

/// 注入一个触摸点事件（坐标是相对窗口的**归一化**坐标 [0,1]）
void PushTouchEvent(u32 id, TouchPhase phase, float xNorm, float yNorm);

/// 注入按键事件（例如音量键/返回键；可选，P3 再用）
void PushKeyEvent(int keyCode, bool down);

} // namespace he::ohos

#endif // HE_PLATFORM_OHOS
