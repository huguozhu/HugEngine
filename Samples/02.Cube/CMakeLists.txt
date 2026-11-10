# ============================================================
# 02.Cube —— 场景与渲染（桌面/鸿蒙共用同一个文件）
#
# 文件结构：
#   1. 可移植 App（IApp + IRenderThreadApp）：立方体 + 球体 + 天空盒 + 触摸相机，
#      桌面与鸿蒙都编这一段；
#   2. `#if defined(HE_DESKTOP_ENTRY)` 段：桌面展示版（多管线/动画/物理/粒子/
#      整套面板），入口是它自己的 main()。鸿蒙 entry 不定义该宏，不含这一段。
# 完整版 02.Cube.cpp 已整体并入本文件。
# ============================================================
if(HUGENGINE_PLATFORM_OHOS)
    return()
endif()

add_executable(02.Cube
    02.Cube.App.cpp
    02.Cube.App.h
    ${CMAKE_SOURCE_DIR}/Samples/Common/SkyboxLoader.cpp
    ${CMAKE_SOURCE_DIR}/Samples/Common/SkyboxLoader.h
    # 展示版里 HE_TRANSIENT_ALIAS_TEST 用到的自检实现（原 02.Cube.cpp 的依赖）
    ${CMAKE_SOURCE_DIR}/Samples/Common/TransientAliasTest.cpp
)

target_compile_definitions(02.Cube PRIVATE
    HE_DESKTOP_ENTRY=1
    HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
)

target_include_directories(02.Cube PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Samples/Common)

target_link_libraries(02.Cube PRIVATE
    HugEngineDesktopHost
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineScene
    HugEngineRender
    HugEnginePhysics
    HugEngineAsset
    HugEngineEditor
    stb
)

add_dependencies(02.Cube CompileShaders)
