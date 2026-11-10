# Platforms/Android —— HugEngine 的 Android 入口

与鸿蒙工程（`Platforms/OHOS`）**完全同构**：样例只实现 `IApp`，平台入口只负责
"窗口挂接 + 事件桥接 + 资产解包"，渲染线程与样例分发在 `Samples/Common/App/MobileHost`。

```
Platforms/Android/
  settings.gradle.kts / build.gradle.kts / gradle.properties / gradle wrapper
  app/
    build.gradle.kts            # externalNativeBuild 指向**仓库根** CMakeLists.txt
    src/main/
      AndroidManifest.xml
      java/com/hugengine/sample/MainActivity.java   # SurfaceView + 触摸 + 样例按钮
      assets/content/…          # 由 sync-assets.ps1 从 Content/ 同步（不入库）
      cpp/
        CMakeLists.txt          # libhgengine.so
        NativeEntry.cpp         # JNI：init/start/resize/touch/screenshot/shutdown
        AssetUnpack.{h,cpp}     # assets/content → filesDir/content（带版本标记）
```

## 构建

前置：Android SDK（`D:\Android\SDK`，platform 36 + build-tools 36）、NDK 27.2、
CMake 3.31.5、**JDK 21**（Android Studio / DevEco 自带的 JBR 25 跑不了 Gradle 8.14）。

```powershell
# 1) 同步资产（01/02 只需天空盒 6 张 PNG）
powershell -ExecutionPolicy Bypass -File Platforms/Android/sync-assets.ps1

# 2) 构建
$env:JAVA_HOME='D:\Android\tools\jdk21\jdk-21.0.13+9'
$env:ANDROID_HOME='D:\Android\SDK'; $env:ANDROID_SDK_ROOT='D:\Android\SDK'
cd Platforms/Android
./gradlew :app:assembleDebug

# 3) 安装
D:\Android\SDK\platform-tools\adb.exe install -r app/build/outputs/apk/debug/app-debug.apk
```

## 与其它平台的差异（踩过的坑）

| 事项 | 原因 | 处理 |
|---|---|---|
| `minSdk = 33` | VMA 静态函数表引用 Vulkan 1.3 的 `vkGetDevice{Buffer,Image}MemoryRequirements`，NDK 的 `libvulkan.so` 只有 **API 33+** 导出 | 首版直接要求 Android 13+ |
| 随仓库的 Vulkan 1.4 头 | NDK r27 头是 1.3，缺 `VK_EXT_device_generated_commands`，引擎 DGC 路径编不过 | `Engine/External/VulkanHeaders/include` 优先于 sysroot |
| 必须 `HUGENGINE_BUILD_EDITOR=ON` | 样例 `#include "Editor/ImGuiIntegration.h"`，编辑器子目录在 OFF 时根本不加入 | Gradle 参数与鸿蒙一致 |
| 不覆盖 `CMAKE_*_OUTPUT_DIRECTORY` | AGP 靠这些变量决定 `.so` 打进 APK 的位置 | 顶层 CMake 里 `if(NOT ANDROID)` 才设置 |
| `imgui_impl_mobile` | 触摸→鼠标的 ImGui 后端鸿蒙/Android 完全一致 | 由 `imgui_impl_ohos` 改名并放开平台守卫 |
| 触摸归一化 | 引擎只接受 [0,1]；鸿蒙曾多除一次导致触摸恒 0 | Java 侧 `event.getX()/view.getWidth()` |

## 已知限制（当前阶段）

- 只编 **arm64-v8a**；要跑 x86_64 模拟器需在 `abiFilters` 里补 `x86_64` 并重装 NDK 支持
- 分享（`ACTION_SEND` + `FileProvider`）尚未接入，对应鸿蒙的 Share Kit，排在 P5
- 资产只带 01/02 需要的天空盒；Sponza（约 56 MB）不随 APK
