#pragma once

#include "Core/Platform.h"

// ============================================================
// InputAndroid.h —— Android 输入（仅保留平台私有的按键注入）
//
// 触摸注入与鸿蒙共用平台无关的 `he::PushTouchPoint`（见 Platform/IInput.h），
// 因此这里只声明 Android 特有的按键入口。
// ============================================================

#if HE_PLATFORM_ANDROID

namespace he::android {

/// 注入一个按键事件（音量键/返回键等；当前样例未用到，保留接口）
void PushKeyEvent(int keyCode, bool down);

} // namespace he::android

#endif // HE_PLATFORM_ANDROID
