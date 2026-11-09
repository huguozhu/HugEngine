#pragma once

// ============================================================
// native_window/external_window.h —— OHOS 原生窗口（stub）
//
// 仅用于 **语法/类型自检**：在 Windows 上把鸿蒙侧源码编一遍，
// 提前发现拼写/类型错误（真机构建必须用 HarmonyOS NDK 的真实头文件）。
// ============================================================

#include <cstdint>

struct OHNativeWindow;

// 真实 SDK：surfaceId 是 uint64_t（ArkTS 侧是字符串，native 里转换）
extern "C" int32_t OH_NativeWindow_CreateNativeWindowFromSurfaceId(uint64_t surfaceId,
                                                                  OHNativeWindow** window);

extern "C" void OH_NativeWindow_DestroyNativeWindow(OHNativeWindow* window);

/// 窗口属性设置（变参）：鸿蒙真实头里是 NativeWindowOperation 枚举
enum NativeWindowOperation {
    SET_BUFFER_GEOMETRY = 0,
};

extern "C" int32_t OH_NativeWindow_NativeWindowHandleOpt(OHNativeWindow* window, int code, ...);
