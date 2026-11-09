#pragma once

#include "Core/Platform.h"   // HE_PLATFORM_*：必须在使用 #if 之前可见

// ============================================================
// RawfileUnpack.h —— rawfile → 沙箱（首次启动解压资产）
//
// 由 NativeEntry 在 init 里调用：ArkTS 传入 ResourceManager 对象，
// 先经 OH_ResourceManager_InitNativeResourceManager 换成 NativeResourceManager*，
// 再递归拷贝到 <filesDir>/content/。
// ============================================================

#include <string>

#if HE_PLATFORM_OHOS

struct NativeResourceManager;

namespace he::ohos {

/// 把 rawfile 里的 rawDir（如 "content"）递归解压到 sandboxDir。
/// mgr 为 nullptr 时返回 false（只记一条警告，不影响渲染继续跑）。
bool UnpackRawfileToSandbox(NativeResourceManager* mgr, const std::string& rawDir,
                            const std::string& sandboxDir);

} // namespace he::ohos

#endif // HE_PLATFORM_OHOS
