# ============================================================
#  Samples/HMOS_App —— 鸿蒙侧共享的「可移植样例 App」目录
#
#  每个子目录里既有 App（与鸿蒙入口共用），也有 Main_Subset.cpp（桌面验证入口）：
#      01.Triangle        三角形 + 旋转 + ImGui
#      02.Cube            立方体 + 球体 + 天空盒 + 触摸相机
#      03.Sponza-Forward  Sponza + 前向管线 + 天空盒
#      04.Sponza-Deferred Sponza + 延迟管线（GBuffer → Lighting → ToneMap）
#
#  为什么单独放一个目录：
#    · 桌面完整样例（Samples/01.Triangle 等）保留各自的全功能 main；
#    · 鸿蒙只跑子集，代码集中在这里，入口（Platforms/OHOS）与桌面回归目标共用同一份；
#    · 桌面构建时这里额外产出 0X.*.Subset 目标，用于验证"鸿蒙将要跑的那份代码"。
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

# ---- 01.Triangle ----
add_executable(01.Triangle.Subset
    01.Triangle/Main_Subset.cpp
    01.Triangle/01.Triangle.App.cpp
    01.Triangle/01.Triangle.App.h
)
target_link_libraries(01.Triangle.Subset PRIVATE ${HMOS_APP_COMMON_LIBS})
target_include_directories(01.Triangle.Subset PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/01.Triangle
    ${CMAKE_SOURCE_DIR}/Samples/Common)
add_dependencies(01.Triangle.Subset CompileShaders)

# ---- 02.Cube ----
add_executable(02.Cube.Subset
    02.Cube/Main_Subset.cpp
    02.Cube/02.Cube.App.cpp
    02.Cube/02.Cube.App.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Common/SkyboxLoader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Common/SkyboxLoader.h
)
target_link_libraries(02.Cube.Subset PRIVATE
    ${HMOS_APP_COMMON_LIBS}
    HugEngineScene
    HugEngineRender
    stb
)
target_include_directories(02.Cube.Subset PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/02.Cube
    ${CMAKE_CURRENT_SOURCE_DIR}/Common
    ${CMAKE_SOURCE_DIR}/Samples/Common)
add_dependencies(02.Cube.Subset CompileShaders)

# ---- 03.Sponza-Forward ----
add_executable(03.Sponza.Subset
    03.Sponza-Forward/Main_Subset.cpp
    03.Sponza-Forward/03.Sponza.App.cpp
    03.Sponza-Forward/03.Sponza.App.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Common/SkyboxLoader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Common/SkyboxLoader.h
)
target_link_libraries(03.Sponza.Subset PRIVATE
    ${HMOS_APP_COMMON_LIBS}
    HugEngineScene
    HugEngineRender
    HugEngineAsset
    stb
)
target_include_directories(03.Sponza.Subset PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/03.Sponza-Forward
    ${CMAKE_CURRENT_SOURCE_DIR}/Common
    ${CMAKE_SOURCE_DIR}/Samples/Common)
target_compile_definitions(03.Sponza.Subset PRIVATE
    HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
)
add_dependencies(03.Sponza.Subset CompileShaders)

# ---- 04.Sponza-Deferred ----
add_executable(04.Deferred.Subset
    04.Sponza-Deferred/Main_Subset.cpp
    04.Sponza-Deferred/04.Deferred.App.cpp
    04.Sponza-Deferred/04.Deferred.App.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Common/SkyboxLoader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Common/SkyboxLoader.h
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
    ${CMAKE_CURRENT_SOURCE_DIR}/Common
    ${CMAKE_SOURCE_DIR}/Samples/Common)
target_compile_definitions(04.Deferred.Subset PRIVATE
    HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
)
add_dependencies(04.Deferred.Subset CompileShaders)
