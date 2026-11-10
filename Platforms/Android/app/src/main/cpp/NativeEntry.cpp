// ============================================================
// NativeEntry.cpp —— Android JNI 桥接（对应鸿蒙的 NativeEntry.cpp / napi）
//
// 只做三件事：
//   1) 把 Java 传进来的 Surface 转成 ANativeWindow*，交给 WindowAndroid；
//   2) 把尺寸/触摸/截图请求翻译成 MobileHost 的调用；
//   3) 首次启动时把 assets/content 解压到 filesDir/content 并设置 VFS 根。
//
// 渲染线程、样例分发、截图转发都在 Samples/Common/App/MobileHost（与鸿蒙共用）。
// ============================================================

#include "Core/Platform.h"

#include <android/asset_manager_jni.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "Core/Log.h"
#include "Platform/IWindow.h"        // SetContentScale
#include "Platform/IVfs.h"
#include "Platform/WindowAndroid.h"

#include "App/MobileHost.h"

#include "AssetUnpack.h"

namespace {

he::u32 g_Width  = 1280;
he::u32 g_Height = 720;

/// 资产版本：Content/ 有变化时 +1（与鸿蒙的 kContentVersion 同义）
constexpr const char* kContentVersion = "1";

/// jstring → std::string（UTF-8）
std::string ToString(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string out(chars ? chars : "");
    if (chars) env->ReleaseStringUTFChars(value, chars);
    return out;
}

} // namespace

extern "C" {

// ---- init(surface, width, height, filesDir, assetManager, density) ----
JNIEXPORT jboolean JNICALL
Java_com_hugengine_sample_MainActivity_nativeInit(JNIEnv* env, jclass, jobject surface,
                                                  jint width, jint height, jstring filesDir,
                                                  jobject assetManager, jfloat density) {
    // 【必须先初始化日志】本函数在 Engine 之前运行，HE_CORE_* 依赖 spdlog logger
    he::Logger::Initialize(he::LogLevel::Info);

    g_Width  = static_cast<he::u32>(width  > 0 ? width  : 1280);
    g_Height = static_cast<he::u32>(height > 0 ? height : 720);
    const std::string dir = ToString(env, filesDir);
    HE_CORE_INFO("[Android] init: surface={} {}x{} dir={} density={}", surface ? "有" : "无",
                 g_Width, g_Height, dir, density);

    // ---- 资产：APK 内 assets/content → filesDir/content（带版本标记）----
    const std::string contentDir  = dir + "/content";
    const std::string versionFile = contentDir + "/.unpack_version";
    bool needUnpack = !std::filesystem::exists(contentDir);
    if (!needUnpack) {
        std::string have;
        if (std::ifstream vf(versionFile); vf) std::getline(vf, have);
        needUnpack = (have != kContentVersion);
    }
    if (needUnpack) {
        AAssetManager* mgr = AAssetManager_fromJava(env, assetManager);
        he::android::UnpackAssetsToSandbox(mgr, "content", contentDir, kContentVersion);
    } else {
        HE_CORE_INFO("[Android] 沙箱资产已是版本 {}，跳过解压", kContentVersion);
    }
    // VFS：只读资产在 content/ 下；配置/截图写 filesDir/
    he::vfs::SetRoots(contentDir + "/", dir + "/");

    // ---- 原生窗口 ----
    ANativeWindow* window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    if (!window) {
        HE_CORE_ERROR("[Android] Surface 为空，无法创建 ANativeWindow");
        return JNI_FALSE;
    }

    // 重复 init（Activity 重建 / 切后台回来）：先停旧实例，避免两个线程抢同一窗口
    if (he::samples::mobile::IsRunning()) {
        HE_CORE_INFO("[Android] 渲染线程仍在运行：先停止旧实例再重建");
        he::samples::mobile::Stop();
    }

    he::SetContentScale(static_cast<he::f32>(density > 0.0f ? density : 1.0f));
    he::AttachNativeWindow(window, g_Width, g_Height);
    he::samples::mobile::Start(g_Width, g_Height, 1, "Android");   // 默认 01.Triangle
    return JNI_TRUE;
}

// ---- start(mode)：切换样例（1 = 01.Triangle，2 = 02.Cube）----
JNIEXPORT void JNICALL
Java_com_hugengine_sample_MainActivity_nativeStart(JNIEnv*, jclass, jint mode) {
    HE_CORE_INFO("[Android] 切换样例: {}（当前 {}）", mode, he::samples::mobile::CurrentMode());
    if (he::samples::mobile::IsRunning()) {
        he::samples::mobile::Stop();
    }
    he::samples::mobile::Start(g_Width, g_Height, static_cast<int>(mode), "Android");
}

// ---- resize(width, height)：旋转 / 分屏 ----
JNIEXPORT void JNICALL
Java_com_hugengine_sample_MainActivity_nativeResize(JNIEnv*, jclass, jint width, jint height) {
    if (width <= 0 || height <= 0) return;
    g_Width  = static_cast<he::u32>(width);
    g_Height = static_cast<he::u32>(height);
    he::AttachNativeWindow(nullptr, g_Width, g_Height);   // 只更新尺寸（句柄保持）
    he::samples::mobile::Resize(g_Width, g_Height);
}

// ---- touch(id, phase, xNorm, yNorm)：坐标是归一化 [0,1] ----
JNIEXPORT void JNICALL
Java_com_hugengine_sample_MainActivity_nativeTouch(JNIEnv*, jclass, jint id, jint phase,
                                                   jfloat xNorm, jfloat yNorm) {
    he::samples::mobile::Touch(static_cast<he::u32>(id), static_cast<int>(phase), xNorm, yNorm);
}

// ---- screenshot(path)：截下一帧到 path（PNG），供 Java 侧调系统分享 ----
JNIEXPORT jboolean JNICALL
Java_com_hugengine_sample_MainActivity_nativeScreenshot(JNIEnv* env, jclass, jstring path) {
    const std::string p = ToString(env, path);
    const bool ok = he::samples::mobile::Screenshot(p.c_str());
    HE_CORE_INFO("[Android] 截图请求: {} → {}", p, ok ? "已排队" : "失败");
    return ok ? JNI_TRUE : JNI_FALSE;
}

// ---- shutdown()：Activity 销毁 / surfaceDestroyed ----
JNIEXPORT void JNICALL
Java_com_hugengine_sample_MainActivity_nativeShutdown(JNIEnv*, jclass) {
    if (he::samples::mobile::IsRunning()) he::samples::mobile::Stop();
    he::DetachNativeWindow();
    HE_CORE_INFO("[Android] 已停止渲染并断开窗口");
}

} // extern "C"
