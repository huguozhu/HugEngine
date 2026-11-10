// ============================================================
// AppMain.cpp —— 鸿蒙端入口薄壳（渲染循环已上收到 Samples/Common/App/MobileHost）
//
// 本文件只做两件事：
//   1) 把鸿蒙的生命周期调用翻译成 MobileHost 的调用；
//   2) 负责"切换样例"时先停旧实例再起新实例（原生窗口由 WindowOHOS 保持）。
//
// 线程、resize 队列、触摸注入、截图转发都在 MobileHost 里，Android 入口
// （Platforms/Android/app/src/main/cpp/NativeEntry.cpp）复用同一份实现。
// ============================================================

#include "Core/Platform.h"

#include "AppMain.h"

#if HE_PLATFORM_OHOS

#include "App/MobileHost.h"   // Samples/Common

namespace he::ohos {

void AppStart(u32 width, u32 height, int mode) {
    samples::mobile::Start(width, height, mode, "OHOS");
}

int AppCurrentMode() {
    return samples::mobile::CurrentMode();
}

bool AppScreenshot(const char* path) {
    return samples::mobile::Screenshot(path);
}

void AppStop() {
    samples::mobile::Stop();
}

bool AppIsRunning() {
    return samples::mobile::IsRunning();
}

void AppResize(u32 width, u32 height) {
    samples::mobile::Resize(width, height);
}

// 【坐标契约】ArkTS 侧传进来的是**归一化**坐标 [0,1]，这里原样转交。
void AppTouch(u32 id, int phase, float xNorm, float yNorm) {
    samples::mobile::Touch(id, phase, xNorm, yNorm);
}

} // namespace he::ohos

#endif // HE_PLATFORM_OHOS
