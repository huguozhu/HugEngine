# 01.Triangle —— 使用 HugEngine RHI 渲染三角形
#
# 桌面入口只负责引擎循环与后端热切换；渲染代码与鸿蒙共用
# （Samples/HMOS_App/01.Triangle/01.Triangle.App.cpp）。
set(HMOS_APP_DIR "${CMAKE_SOURCE_DIR}/Samples/HMOS_App/01.Triangle")

add_executable(01.Triangle
    01.Triangle.cpp
    ${HMOS_APP_DIR}/01.Triangle.App.cpp
    ${HMOS_APP_DIR}/01.Triangle.App.h
)

target_include_directories(01.Triangle PRIVATE ${HMOS_APP_DIR})

target_link_libraries(01.Triangle PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineEditor
)

# 依赖 CompileShaders 确保 .spv.h 等着色器头文件已生成
add_dependencies(01.Triangle CompileShaders)
