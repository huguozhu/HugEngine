#pragma once

#include "Core/Platform.h"   // HE_PLATFORM_*：必须在使用 #if 之前可见

// ============================================================
// InputOHOS.h —— 鸿蒙输入（仅保留平台私有的按键注入）
//
// 触摸注入已经上收到平台无关的 `he::PushTouchPoint`（见 Platform/IInput.h），
// 这样 MobileHost 这类共享宿主不需要知道"当前是鸿蒙还是 Android"。
// ============================================================

#if HE_PLATFORM_OHOS

namespace he::ohos {

/// 注入一个按键事件（音量键/返回键等；当前样例未用到，保留接口）
void PushKeyEvent(int keyCode, bool down);

} // namespace he::ohos

#endif // HE_PLATFORM_OHOS
