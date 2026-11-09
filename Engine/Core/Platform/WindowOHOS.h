#pragma once

#include "Core/Platform.h"   // HE_PLATFORM_*：必须在使用 #if 之前可见

// ============================================================
// WindowOHOS.h —— 鸿蒙原生窗口挂接（XComponent → OHNativeWindow）
//
// ArkTS 侧 XComponent 的 surface 由 NativeEntry 转成 OHNativeWindow 后调用
// AttachNativeWindow 交给引擎；WindowOHOS 用它创建 Vulkan 表面。
// ============================================================

#include "Core/Types.h"

#if HE_PLATFORM_OHOS

// 【不要前向声明 OHNativeWindow】SDK 里它是 `typedef struct NativeWindow OHNativeWindow;`，
// 自己写 `struct OHNativeWindow;` 会与之冲突（typedef redefinition），必须直接包含 SDK 头。
#include <native_window/external_window.h>

namespace he {

/// surfaceCreated / surfaceChanged 时调用（窗口句柄 + 像素尺寸）
void AttachNativeWindow(OHNativeWindow* window, u32 width, u32 height);

/// surfaceDestroyed 时调用：置空窗口，渲染循环会自行暂停
void DetachNativeWindow();

/// 设置内容缩放（ArkTS 侧用 display.densityPixels 传进来；ImGui 按此放大字体与间距）
void SetContentScale(f32 scale);

} // namespace he

#endif // HE_PLATFORM_OHOS
