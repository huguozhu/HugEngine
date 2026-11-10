# ============================================================
#  Samples/HMOS_App —— 鸿蒙侧共享的「可移植样例 App」目录
#
#  每个子目录里既有 App（与鸿蒙入口共用），也有 Main_Subset.cpp（桌面验证入口）：
#      02.Cube            立方体 + 球体 + 天空盒 + 触摸相机
#      03.Sponza-Forward  Sponza + 前向管线 + 天空盒
#      04.Sponza-Deferred Sponza + 延迟管线（GBuffer → Lighting → ToneMap）
#
#  为什么单独放一个目录：
#    · 桌面完整样例（Samples/02.Cube 等）保留各自的全功能 main；
#    · 鸿蒙只跑子集，代码集中在这里，入口（Platforms/OHOS）与桌面回归目标共用同一份；
#    · 桌面构建时这里额外产出 0X.*.Subset 目标，用于验证"鸿蒙将要跑的那份代码"。
#
#  01.Triangle 已经迁到新布局（2026-11-10）：
#    · 场景/渲染：Samples/01.Triangle/01.Triangle.App.{h,cpp}（实现 IApp）
#    · 桌面入口：Samples/01.Triangle/Main.Win32.cpp + Platforms/Win32/DesktopHost
#    · 鸿蒙入口：Platforms/OHOS/entry 直接编同一份 App
#  其余样例后续按同一模式迁移。
# ============================================================

# 桌面才会产出验证目标：鸿蒙构建里这些 App 由 Platforms/OHOS 的 entry 直接编译
if(HUGENGINE_PLATFORM_OHOS)
    return()
endif()

set(HMOS_APP_COMMON_LIBS
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineEditor
)

# ---- 02.Cube ----
add_executable(02.Cube.Subset
    02.Cube/Main_Subset.cpp
    02.Cube/02.Cube.App.cpp
    02.Cube/02.Cube.App.h
    ${CMAKE_SOURCE_DIR}/Samples/Common/SkyboxLoader.cpp
    ${CMAKE_SOURCE_DIR}/Samples/Common/SkyboxLoader.h
)
target_link_libraries(02.Cube.Subset PRIVATE
    ${HMOS_APP_COMMON_LIBS}
    HugEngineScene
    HugEngineRender
    stb
)
target_include_directories(02.Cube.Subset PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/02.Cube
    ${CMAKE_SOURCE_DIR}/Samples/Common)
add_dependencies(02.Cube.Subset CompileShaders)

# ---- 04.Sponza-Deferred ----
add_executable(04.Deferred.Subset
    04.Sponza-Deferred/Main_Subset.cpp
    04.Sponza-Deferred/04.Deferred.App.cpp
    04.Sponza-Deferred/04.Deferred.App.h
    ${CMAKE_SOURCE_DIR}/Samples/Common/SkyboxLoader.cpp
    ${CMAKE_SOURCE_DIR}/Samples/Common/SkyboxLoader.h
)
target_link_libraries(04.Deferred.Subset PRIVATE
    ${HMOS_APP_COMMON_LIBS}
    HugEngineScene
    HugEngineRender
    HugEngineAsset
    stb
)
target_include_directories(04.Deferred.Subset PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/04.Sponza-Deferred
    ${CMAKE_SOURCE_DIR}/Samples/Common)
target_compile_definitions(04.Deferred.Subset PRIVATE
    HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
)
add_dependencies(04.Deferred.Subset CompileShaders)
