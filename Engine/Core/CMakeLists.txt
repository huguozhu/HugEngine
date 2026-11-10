# Core — Platform abstraction, Math, Memory, Containers, Threading (L0)
# Phase 1

add_library(HugEngineCore STATIC)

set(CORE_SOURCES
    Core/Engine.cpp
    Core/Engine.h
    Core/RenderThreadingMode.h
    Core/Log.cpp
    Core/Log.h
    Core/CrashHandler.h
    Core/Assert.h
    Core/CVar.cpp
    Core/CVar.h
    Core/Core.h
    Core/Platform.h
    Core/Types.h
    Math/Geometry.cpp
    Math/Geometry.h
    Math/Math.h
    Containers/Array.h
    Memory/Allocator.h
    Platform/Time.cpp
    Platform/Vfs.cpp
    Platform/IWindow.h
    Platform/IInput.h
    Platform/IVfs.h
    Platform/Time.h
    Platform/WindowWin32.h
    Threading/JobSystem.cpp
    Threading/JobSystem.h
)

target_sources(HugEngineCore PRIVATE ${CORE_SOURCES})

# IDE 目录树：按磁盘目录分组，.h 和 .cpp 合并在一起
huge_source_group(${CORE_SOURCES})

# 旧样例的 HUGE_CONTENT_DIR 与 VFS 的默认资产根同源：统一在这里注入一次。
# 鸿蒙构建（OHOS）不注入：根目录由 NativeEntry 解压 rawfile 后调用 vfs::SetRoots 设置。
if(WIN32)
    target_compile_definitions(HugEngineCore
        PUBLIC HUGENGINE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/")
endif()
target_include_directories(HugEngineCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# 鸿蒙（HarmonyOS NEXT）：窗口来自 XComponent 的 OHNativeWindow，输入来自触摸事件
if(HUGENGINE_PLATFORM_OHOS)
    target_sources(HugEngineCore PRIVATE
        Platform/WindowOHOS.cpp
        Platform/InputOHOS.cpp
        Platform/InputOHOS.h)
endif()

# Android：窗口来自 SurfaceView 的 ANativeWindow，输入来自 JNI 转发的触摸事件
# （触摸语义与鸿蒙一致，二者共用 he::PushTouchPoint，见 Platform/IInput.h）
if(HUGENGINE_PLATFORM_ANDROID)
    target_sources(HugEngineCore PRIVATE
        Platform/WindowAndroid.cpp
        Platform/WindowAndroid.h
        Platform/InputAndroid.cpp
        Platform/InputAndroid.h)
endif()
# Windows 专属：GLFW 窗口/输入实现 + 崩溃处理器（DbgHelp）
if(WIN32)
    target_sources(HugEngineCore PRIVATE
        Platform/WindowWin32.cpp
        Platform/InputWin32.cpp
        Core/CrashHandler.cpp)
    target_link_libraries(HugEngineCore PUBLIC glfw)
endif()

target_link_libraries(HugEngineCore PUBLIC
    glm::glm
    spdlog::spdlog
    Taskflow
)

# 崩溃处理器需要 DbgHelp（调用栈符号化 + MiniDumpWriteDump）
if(WIN32)
    target_link_libraries(HugEngineCore PUBLIC dbghelp)
endif()

# 鸿蒙：Log.cpp 的 hilog sink 需要系统库 libhilog_ndk.z.so
if(HUGENGINE_PLATFORM_OHOS)
    find_library(OHOS_LIB_hilog_ndk NAMES hilog_ndk.z REQUIRED)
    target_link_libraries(HugEngineCore PUBLIC ${OHOS_LIB_hilog_ndk})
endif()

# Android：Log.cpp 的 logcat sink 需要 liblog（NDK sysroot 自带）
if(HUGENGINE_PLATFORM_ANDROID)
    find_library(ANDROID_LIB_log NAMES log REQUIRED)
    target_link_libraries(HugEngineCore PUBLIC ${ANDROID_LIB_log})
endif()