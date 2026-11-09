#pragma once

#include "Core/Platform.h"   // HE_PLATFORM_*：必须在使用 #if 之前可见

// ============================================================
// AppMain.h —— 鸿蒙端渲染循环（P1：Vulkan 三角形冒烟）
//
// 生命周期由 NativeEntry 驱动：
//   AttachNativeWindow（Core）→ AppStart（起渲染线程）→ … → AppStop（停线程并销毁设备）
// 事件转发：AppResize / AppTouch。
// ============================================================

#include "Core/Types.h"

#if HE_PLATFORM_OHOS

namespace he::ohos {

/// 起渲染线程（调用前必须已 AttachNativeWindow）
void AppStart(u32 width, u32 height);

/// 停渲染线程并等待其退出（设备/交换链在线程内销毁）
void AppStop();

/// 渲染线程是否在跑（surface 重建时用于判断是否需要先停旧实例）
bool AppIsRunning();

/// 画布尺寸变化（旋转/分屏）
void AppResize(u32 width, u32 height);

/// 触摸事件（坐标是像素，内部换算成归一化坐标）
void AppTouch(u32 id, int phase, float xPixels, float yPixels);

} // namespace he::ohos

#endif // HE_PLATFORM_OHOS
