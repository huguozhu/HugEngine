# ============================================================
#  Platforms/OHOS/stubcheck/CMakeLists.txt
#
#  鸿蒙侧源码的"编译自检"目标（仅桌面构建）：
#    · 用 Platforms/OHOS/stubcheck/include 里的 stub 头替代 NDK 头
#    · 用 -DHE_PLATFORM_OHOS_STUBCHECK=1 让 Platform.h 走鸿蒙分支
#    · 只编译、不链接（EXCLUDE_FROM_ALL）：把拼写/类型/签名错误提前暴露
#
#  真机构建仍然走 Platforms/OHOS/entry/src/main/cpp/CMakeLists.txt + NDK 真实头文件。
# ============================================================

set(OHOS_STUB_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(OhosStubCheck OBJECT
    ${CMAKE_SOURCE_DIR}/Engine/Core/Platform/WindowOHOS.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Core/Platform/InputOHOS.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Editor/Editor/imgui_impl_ohos.cpp
    ${CMAKE_SOURCE_DIR}/Engine/RHI/Vulkan/VulkanPlatform.cpp
    ${CMAKE_SOURCE_DIR}/Platforms/OHOS/entry/src/main/cpp/NativeEntry.cpp
    ${CMAKE_SOURCE_DIR}/Platforms/OHOS/entry/src/main/cpp/RawfileUnpack.cpp
    ${CMAKE_SOURCE_DIR}/Platforms/OHOS/entry/src/main/cpp/AppMain.cpp
)

target_compile_definitions(OhosStubCheck PRIVATE
    HE_PLATFORM_OHOS_STUBCHECK=1
)

target_include_directories(OhosStubCheck PRIVATE
    ${OHOS_STUB_INCLUDE}
    ${CMAKE_SOURCE_DIR}/Engine/Core
    ${CMAKE_SOURCE_DIR}/Engine/RHI
    ${CMAKE_SOURCE_DIR}/Engine/Shader/Public
    ${CMAKE_SOURCE_DIR}/Engine/Shader/Shaders
    ${CMAKE_SOURCE_DIR}/Engine/Scene
    ${CMAKE_SOURCE_DIR}/Engine/Asset
    ${CMAKE_SOURCE_DIR}/Engine/Render
    ${CMAKE_SOURCE_DIR}/Engine/Editor
    # 可移植 App 头：Entry 的 AppMain.cpp 同时引用四个样例
    ${CMAKE_SOURCE_DIR}/Samples/01.Triangle
    ${CMAKE_SOURCE_DIR}/Samples/HMOS_App/02.Cube
    ${CMAKE_SOURCE_DIR}/Samples/HMOS_App/03.Sponza-Forward
    ${CMAKE_SOURCE_DIR}/Samples/HMOS_App/04.Sponza-Deferred
    ${CMAKE_SOURCE_DIR}/Samples/HMOS_App/Common
    ${CMAKE_SOURCE_DIR}/Samples/Common
)

# 引擎 target 提供第三方依赖（glm/spdlog/taskflow/imgui）与 Vulkan 头
target_link_libraries(OhosStubCheck PRIVATE
    HugEngineCore
    imgui
    Vulkan::Vulkan
)

set_target_properties(OhosStubCheck PROPERTIES
    FOLDER "Platforms/OHOS"
    EXCLUDE_FROM_ALL TRUE
)
