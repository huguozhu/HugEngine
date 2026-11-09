// ============================================================

#include "Core/Platform.h"   // HE_PLATFORM_*：必须在使用 #if 之前可见
// NativeEntry.cpp —— 鸿蒙 napi 入口（ArkTS ↔ 引擎）

//

// 导出的接口（ArkTS 侧 import native from 'libentry.so'）：

//   init(surfaceId: string, width: number, height: number, filesDir: string): void

//   resize(width: number, height: number): void

//   touch(id: number, phase: number, x: number, y: number): void

//   shutdown(): void

//

// 关键点：

//   · surfaceId 来自 XComponentController.getXComponentSurfaceId()；

//     用 OH_NativeWindow_CreateNativeWindowFromSurfaceId 换成 OHNativeWindow*，

//     再 AttachNativeWindow 交给引擎（WindowOHOS）。

//   · filesDir 是应用沙箱目录：P2 会把 rawfile 里的资产解压到这里，

//     并调用 he::vfs::SetRoots(沙箱资产目录, 沙箱可写目录)。

//   · 渲染线程由 AppStart 创建；napi 回调只做转发，不碰 RHI。

// ============================================================


#include "AppMain.h"

#if HE_PLATFORM_OHOS

#include "Core/Log.h"
#include "Platform/IVfs.h"
#include "Platform/WindowOHOS.h"

#include "RawfileUnpack.h"
#include <rawfile/raw_file_manager.h>   // OH_ResourceManager_InitNativeResourceManager
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <native_window/external_window.h>
#include <napi/native_api.h>

#include <stdexcept>
#include <string>

namespace {

he::u32 g_Width  = 960;
he::u32 g_Height = 540;

/// 从 napi 值取 UTF-8 字符串

std::string GetString(napi_env env, napi_value value) {
    size_t len = 0;
    napi_get_value_string_utf8(env, value, nullptr, 0, &len);
    std::string out(len, '\0');
    napi_get_value_string_utf8(env, value, out.data(), len + 1, &len);
    out.resize(len);
    return out;
}

he::u32 GetU32(napi_env env, napi_value value) {
    uint32_t v = 0;
    napi_get_value_uint32(env, value, &v);
    return static_cast<he::u32>(v);
}

double GetDouble(napi_env env, napi_value value) {
    double v = 0.0;
    napi_get_value_double(env, value, &v);
    return v;
}

// ---- start(mode)：切换样例（1 = 01.Triangle，2 = 02.Cube）----
// 渲染线程会整份重建（设备/交换链/管线），但 XComponent 的原生窗口保持不变。
napi_value NativeStart(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    const int mode = (argc >= 1) ? static_cast<int>(GetDouble(env, argv[0])) : 1;
    HE_CORE_INFO("[OHOS] 切换样例: {}（当前 {}）", mode == 2 ? "02.Cube" : "01.Triangle",
                 he::ohos::AppCurrentMode());
    if (he::ohos::AppIsRunning()) {
        he::ohos::AppStop();
    }
    he::ohos::AppStart(g_Width, g_Height, mode);
    napi_value undef = nullptr;
    napi_get_undefined(env, &undef);
    return undef;
}

// ---- setUiScale(scale)：屏幕密度，ImGui 用它在高 DPI 屏上放大字体与控件 ----
napi_value NativeSetUiScale(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    const double scale = (argc >= 1) ? GetDouble(env, argv[0]) : 1.0;
    he::SetContentScale(static_cast<he::f32>(scale));
    return nullptr;
}

// ---- init(surfaceId, width, height, filesDir, resourceManager) ----

napi_value NativeInit(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value argv[5] = { nullptr };
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 5) {
        HE_CORE_ERROR("[OHOS] init 参数不足（需要 surfaceId, width, height, filesDir, resourceManager）");
        return nullptr;
    }

    // 【必须先初始化日志】NativeEntry 在 Engine 之前运行，而 HE_CORE_* 依赖 spdlog logger；
    // 未初始化就打印会空指针崩溃（真机实测：SIGSEGV in spdlog::logger::should_log）。
    he::Logger::Initialize(he::LogLevel::Info);

    const std::string surfaceId = GetString(env, argv[0]);
    g_Width  = GetU32(env, argv[1]);
    g_Height = GetU32(env, argv[2]);
    const std::string filesDir = GetString(env, argv[3]);

    // ---- 资产：rawfile → 沙箱 ----
    // 【版本标记】只判断"目录存在"会导致换了 rawfile 资产后旧沙箱不更新（实测：加了
    // Sponza 资源后手机上仍只有旧文件）。这里写一个版本文件，版本不一致就重解压。
    constexpr const char* kContentVersion = "2";   // 资产有变更时 +1
    const std::string contentDir  = filesDir + "/content";
    const std::string versionFile = contentDir + "/.unpack_version";

    bool needUnpack = !std::filesystem::exists(contentDir);
    if (!needUnpack) {
        std::string have;
        if (std::ifstream vf(versionFile); vf) { std::getline(vf, have); }
        needUnpack = (have != kContentVersion);
    }
    if (needUnpack) {
        // ArkTS 传入的 ResourceManager（napi 对象）→ NDK 句柄，再交给解压器
        NativeResourceManager* mgr = OH_ResourceManager_InitNativeResourceManager(env, argv[4]);
        HE_CORE_INFO("[OHOS] 解压 rawfile 资产（版本 {}）", kContentVersion);
        he::ohos::UnpackRawfileToSandbox(mgr, "content", contentDir);
        std::ofstream vf(versionFile, std::ios::trunc);
        if (vf) vf << kContentVersion;
    } else {
        HE_CORE_INFO("[OHOS] 沙箱资产已是版本 {}，跳过解压: {}", kContentVersion, contentDir);
    }
    // VFS：只读资产在 content/ 下；配置/日志写 filesDir/
    he::vfs::SetRoots(contentDir + "/", filesDir + "/");

    OHNativeWindow* nativeWindow = nullptr;
    // 【真实 SDK 签名】OH_NativeWindow_CreateNativeWindowFromSurfaceId(uint64_t surfaceId, OHNativeWindow**)
    // ArkTS 的 getXComponentSurfaceId() 给出的是字符串形式的 64 位 id，这里先转成整数。
    uint64_t surfaceHandle = 0;
    try {
        surfaceHandle = std::stoull(surfaceId);
    } catch (const std::exception& e) {
        HE_CORE_ERROR("[OHOS] surfaceId 解析失败: '{}'（{}）", surfaceId, e.what());
        return nullptr;
    }
    const int32_t rc = OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceHandle, &nativeWindow);
    if (rc != 0 || nativeWindow == nullptr) {
        HE_CORE_ERROR("[OHOS] 从 surfaceId 创建 OHNativeWindow 失败: rc={}", rc);
        return nullptr;
    }

    // 重复 init（surface 重建 / 切后台再回来）：先把旧实例停干净，避免两个渲染线程抢同一个窗口
    if (he::ohos::AppIsRunning()) {
        HE_CORE_INFO("[OHOS] 检测到渲染线程仍在运行：先停止旧实例再重建");
        he::ohos::AppStop();
    }

    HE_CORE_INFO("[OHOS] surfaceId={} 窗口={}x{} filesDir={}", surfaceId, g_Width, g_Height, filesDir);
    he::AttachNativeWindow(nativeWindow, g_Width, g_Height);
    he::ohos::AppStart(g_Width, g_Height, 1);   // 默认 01.Triangle

    napi_value undef = nullptr;
    napi_get_undefined(env, &undef);
    return undef;
}

// ---- resize(width, height) ----

napi_value NativeResize(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { nullptr };
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc >= 2) {
        g_Width  = GetU32(env, argv[0]);
        g_Height = GetU32(env, argv[1]);
        he::AttachNativeWindow(nullptr, g_Width, g_Height);   // 只更新尺寸（窗口句柄保持）
        he::ohos::AppResize(g_Width, g_Height);
    }
    napi_value undef = nullptr;
    napi_get_undefined(env, &undef);
    return undef;
}

// ---- touch(id, phase, x, y) ----

napi_value NativeTouch(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4] = { nullptr };
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc >= 4) {
        const he::u32 id  = GetU32(env, argv[0]);
        const int  phase = static_cast<int>(GetDouble(env, argv[1]));
        const auto x     = static_cast<float>(GetDouble(env, argv[2]));
        const auto y     = static_cast<float>(GetDouble(env, argv[3]));
        he::ohos::AppTouch(id, phase, x, y);
    }
    napi_value undef = nullptr;
    napi_get_undefined(env, &undef);
    return undef;
}

// ---- shutdown() ----

napi_value NativeShutdown(napi_env env, napi_callback_info) {
    if (he::ohos::AppIsRunning()) he::ohos::AppStop();
    he::DetachNativeWindow();
    HE_CORE_INFO("[OHOS] 已停止渲染并断开窗口");
    napi_value undef = nullptr;
    napi_get_undefined(env, &undef);
    return undef;
}

napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        { "init",     nullptr, NativeInit,     nullptr, nullptr, nullptr, napi_default, nullptr },
        { "start",    nullptr, NativeStart,    nullptr, nullptr, nullptr, napi_default, nullptr },
        { "resize",   nullptr, NativeResize,   nullptr, nullptr, nullptr, napi_default, nullptr },
        { "touch",    nullptr, NativeTouch,    nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setUiScale", nullptr, NativeSetUiScale, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "shutdown", nullptr, NativeShutdown, nullptr, nullptr, nullptr, napi_default, nullptr },
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    // 【模块加载期就要有日志】Init 在 ArkTS `import native from 'libentry.so'` 时执行，
    // 早于 pages/Index 的 native.init；此时 Engine 还没建，logger 必须在这里补上，
    // 否则本条 HE_CORE_INFO 会空指针崩溃（真机实测：SIGSEGV in spdlog::logger::should_log）。
    he::Logger::Initialize(he::LogLevel::Info);
    HE_CORE_INFO("[OHOS] libentry.so 已加载（init/start/resize/touch/setUiScale/shutdown）");
    return exports;
}

} // namespace

static napi_module g_Module = {
    .nm_version = 1,
    .nm_flags   = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "entry",
    .nm_priv    = nullptr,
    .reserved   = { nullptr },
};

// 模块自动注册：鸿蒙 NDK 用 clang，__attribute__((constructor)) 有效；
// Windows 侧语法自检（MSVC）不支持该属性，退化成普通函数即可（不参与运行）。
#if defined(__GNUC__) || defined(__clang__)
extern "C" __attribute__((constructor))
#endif
void RegisterEntryModule() {
    napi_module_register(&g_Module);
}

#endif // HE_PLATFORM_OHOS
