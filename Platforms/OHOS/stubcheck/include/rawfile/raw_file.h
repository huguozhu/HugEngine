#pragma once

// ============================================================
// rawfile/raw_file.h —— OHOS rawfile（stub，仅供 Windows 侧语法自检）
// ============================================================

#include <cstddef>

struct RawFile;
struct RawDir;
struct NativeResourceManager;

extern "C" RawFile* OH_ResourceManager_OpenRawFile(NativeResourceManager* mgr, const char* path);
extern "C" long     OH_ResourceManager_GetRawFileSize(RawFile* file);
extern "C" long     OH_ResourceManager_ReadRawFile(RawFile* file, void* buf, size_t len);
extern "C" void     OH_ResourceManager_CloseRawFile(RawFile* file);

extern "C" RawDir*     OH_ResourceManager_OpenRawDir(NativeResourceManager* mgr, const char* path);
extern "C" int         OH_ResourceManager_GetRawFileCount(RawDir* dir);
extern "C" const char* OH_ResourceManager_GetRawFileName(RawDir* dir, int index);
extern "C" void        OH_ResourceManager_CloseRawDir(RawDir* dir);
