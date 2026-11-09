# ============================================================
#  Platforms/OHOS/entry/src/main/cpp/CMakeLists.txt
#
#  【本文件不是顶层工程】它由仓库根 CMakeLists 在
#  `-DHUGENGINE_BUILD_OHOS_ENTRY=ON` 时 add_subdirectory 进来：
#     cmake -S <repo> -B build-ohos -G Ninja \
#           -DCMAKE_TOOLCHAIN_FILE=cmake/OHOS.toolchain.cmake -DOHOS_NDK=<sdk>/native \
#           -DHUGENGINE_BUILD_OHOS_ENTRY=ON -DHUGENGINE_BUILD_SAMPLES=OFF
#
#  【为什么不让本文件当顶层】引擎/第三方（spdlog/glm/…）大量使用
#  ${CMAKE_SOURCE_DIR} 表示"仓库根"。若把引擎当子目录加入，CMAKE_SOURCE_DIR 会指向
#  本目录，所有源码/头文件路径全部错位（实测：VMA 头找不到）。
# ============================================================

set(CUBE02_APP_DIR     "${CMAKE_SOURCE_DIR}/Samples/HMOS_App/02.Cube")
set(TRIANGLE01_APP_DIR "${CMAKE_SOURCE_DIR}/Samples/HMOS_App/01.Triangle")
set(SPONZA03_APP_DIR   "${CMAKE_SOURCE_DIR}/Samples/HMOS_App/03.Sponza-Forward")
set(SPONZA04_APP_DIR   "${CMAKE_SOURCE_DIR}/Samples/HMOS_App/04.Sponza-Deferred")

add_library(entry SHARED
    NativeEntry.cpp
    RawfileUnpack.cpp
    RawfileUnpack.h
    AppMain.cpp
    AppMain.h
    ${CUBE02_APP_DIR}/02.Cube.App.cpp
    ${CUBE02_APP_DIR}/02.Cube.App.h
    ${TRIANGLE01_APP_DIR}/01.Triangle.App.cpp
    ${TRIANGLE01_APP_DIR}/01.Triangle.App.h
    ${SPONZA03_APP_DIR}/03.Sponza.App.cpp
    ${SPONZA03_APP_DIR}/03.Sponza.App.h
    ${SPONZA04_APP_DIR}/04.Deferred.App.cpp
    ${SPONZA04_APP_DIR}/04.Deferred.App.h
)

target_include_directories(entry PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CUBE02_APP_DIR}
    ${TRIANGLE01_APP_DIR}
    ${SPONZA03_APP_DIR}
    ${SPONZA04_APP_DIR}
    ${CMAKE_SOURCE_DIR}/Samples/Common
)

target_link_libraries(entry PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineScene
    HugEngineRender
    HugEngineEditor
    stb
)

# 鸿蒙系统库：用 find_library 解析，避免硬编码 "libxxx.z.so" 名字（不同 SDK 版本略有差异）
foreach(_ohos_lib ace_napi.z hilog_ndk.z native_window rawfile.z ohresmgr)
    find_library(OHOS_LIB_${_ohos_lib} NAMES ${_ohos_lib} REQUIRED)
    target_link_libraries(entry PRIVATE ${OHOS_LIB_${_ohos_lib}})
endforeach()

set_target_properties(entry PROPERTIES
    FOLDER "Platforms/OHOS"
    PREFIX "lib"
    OUTPUT_NAME "entry"
)
