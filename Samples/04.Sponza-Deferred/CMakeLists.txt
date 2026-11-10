# ============================================================
# 04.Sponza-Deferred —— 场景与渲染在 04.Deferred.App.cpp（桌面/鸿蒙共用同一份）
#
# 桌面入口在同一个 .cpp 的 HE_DESKTOP_ENTRY 段里（配置 + Platforms/Win32 宿主）；
# 鸿蒙 entry 编同一份 .cpp 但不定义该宏，入口在 Platforms/OHOS/entry 的 AppMain.cpp。
# 完整版 main 已并入 App（配置读写 + 面板开关 + 渲染线程都在 App/宿主里）。
# ============================================================
if(HUGENGINE_PLATFORM_OHOS)
    return()
endif()

add_executable(04.Sponza-Deferred
    04.Deferred.App.cpp
    04.Deferred.App.h
    ${CMAKE_SOURCE_DIR}/Samples/Common/SkyboxLoader.cpp
    ${CMAKE_SOURCE_DIR}/Samples/Common/SkyboxLoader.h
)

target_compile_definitions(04.Sponza-Deferred PRIVATE
    HE_DESKTOP_ENTRY=1
    HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
)

target_include_directories(04.Sponza-Deferred PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Samples/Common)

target_link_libraries(04.Sponza-Deferred PRIVATE
    HugEngineDesktopHost
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineScene
    HugEngineRender
    HugEngineAsset
    HugEngineEditor
    stb
)

add_dependencies(04.Sponza-Deferred CompileShaders)
