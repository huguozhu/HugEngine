#pragma once

#include "Core/Core.h"

// ============================================================
// MobileHost.h —— 移动端共享宿主（鸿蒙 / Android 共用）
//
// 引擎侧只关心"起一个渲染线程，把窗口/尺寸/触摸/截图转发给 App"，
// 这些逻辑与具体平台无关，因此从鸿蒙的 AppMain.cpp 上提到这里：
//   · 鸿蒙入口（napi）：Platforms/OHOS/entry/src/main/cpp/AppMain.cpp
//   · Android 入口（JNI）：Platforms/Android/app/src/main/cpp/NativeEntry.cpp
// 两边都只做"平台桥接"（把 ArkTS / Java 的事件翻译成这里的函数调用）。
//
// 样例本身仍然只实现 Samples/Common/App/IApp.h，不感知宿主。
// ============================================================

namespace he::samples::mobile {

/// 启动渲染线程。
/// @param width/height 初始像素尺寸（surface 实际尺寸，用于建交换链）
/// @param mode         样例编号：1=01.Triangle，2=02.Cube，3=03.Sponza，4=04.Deferred
/// @param hostName     平台名（写进日志与窗口标题，例如 "OHOS" / "Android"）
void Start(u32 width, u32 height, int mode, const char* hostName);

/// 渲染线程是否在跑
bool IsRunning();

/// 当前样例编号
int CurrentMode();

/// 停止渲染线程（等它退出并 Shutdown 当前 App）
void Stop();

/// 投递尺寸变化（旋转 / 分屏 / 窗口改动），渲染线程下一帧应用
void Resize(u32 width, u32 height);

/// 注入触摸点：坐标为**归一化** [0,1]；phase 取值见 he::TouchPhase（0..3）
void Touch(u32 id, int phase, float xNorm, float yNorm);

/// 请求截取下一帧到 path（PNG）；返回 false 表示当前没有运行中的 App
bool Screenshot(const char* path);

} // namespace he::samples::mobile
