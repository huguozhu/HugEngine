# ============================================================
#  Platforms/Android/app/src/main/cpp/CMakeLists.txt
#
#  Android native 入口（libhgengine.so）。与鸿蒙的 entry 完全同构：
#   · 由仓库根 CMake 在 -DHUGENGINE_BUILD_ANDROID_ENTRY=ON 时 add_subdirectory 进来；
#   · 样例源码直接编译（不建 exe），渲染循环在 Samples/Common/App/MobileHost；
#   · 本文件只多编一个 JNI 桥接（NativeEntry.cpp）与资产解包（AssetUnpack.cpp）。
# ============================================================

set(TRIANGLE01_APP_DIR "${CMAKE_SOURCE_DIR}/Samples/01.Triangle")
set(CUBE02_APP_DIR     "${CMAKE_SOURCE_DIR}/Samples/02.Cube")

add_library(hgengine SHARED
    NativeEntry.cpp
    AssetUnpack.cpp
    AssetUnpack.h
    # 移动端共享宿主（鸿蒙入口用的是同一份实现）
    ${CMAKE_SOURCE_DIR}/Samples/Common/App/MobileHost.cpp
    ${CMAKE_SOURCE_DIR}/Samples/Common/App/MobileHost.h
    # 首版只带 01/02（Sponza 资产约 56MB，暂不随 APK 分发）
    ${TRIANGLE01_APP_DIR}/01.Triangle.App.cpp
    ${TRIANGLE01_APP_DIR}/01.Triangle.App.h
    ${CUBE02_APP_DIR}/02.Cube.App.cpp
    ${CUBE02_APP_DIR}/02.Cube.App.h
    ${CMAKE_SOURCE_DIR}/Samples/Common/SkyboxLoader.cpp
    ${CMAKE_SOURCE_DIR}/Samples/Common/SkyboxLoader.h
)

# HE_MOBILE_MAX_MODE=2：MobileHost 里裁掉 03/04 的分发分支（对应源码也没编进来）
target_compile_definitions(hgengine PRIVATE
    HE_MOBILE_MAX_MODE=2
)

target_include_directories(hgengine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${TRIANGLE01_APP_DIR}
    ${CUBE02_APP_DIR}
    ${CMAKE_SOURCE_DIR}/Samples/Common
)

target_link_libraries(hgengine PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineScene
    HugEngineRender
    HugEngineEditor
    stb
)

# Android 系统库：JNI 由 NDK 自动链接 liblog/libandroid；这里显式列出便于排查
find_library(ANDROID_LIB_android NAMES android REQUIRED)
find_library(ANDROID_LIB_log     NAMES log     REQUIRED)
target_link_libraries(hgengine PRIVATE ${ANDROID_LIB_android} ${ANDROID_LIB_log})

set_target_properties(hgengine PROPERTIES
    FOLDER "Platforms/Android"
    PREFIX "lib"
    OUTPUT_NAME "hgengine"
)
