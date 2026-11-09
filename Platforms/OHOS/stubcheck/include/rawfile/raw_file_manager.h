#pragma once

// ============================================================
// rawfile/raw_file_manager.h —— OHOS ResourceManager（stub，仅供语法自检）
// ============================================================

#include <napi/native_api.h>

struct NativeResourceManager;

extern "C" NativeResourceManager* OH_ResourceManager_InitNativeResourceManager(
    napi_env env, napi_value jsResourceManager);
