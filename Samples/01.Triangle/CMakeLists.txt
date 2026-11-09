# 01.Triangle — 使用 HugEngine RHI 渲染三角形
add_executable(01.Triangle
    01.Triangle.cpp
    App/01.Triangle.App.cpp
    App/01.Triangle.App.h
)

target_include_directories(01.Triangle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/App)

target_link_libraries(01.Triangle PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineEditor
)

# 依赖 CompileShaders 确保 .spv.h 着色器头文件已生成
add_dependencies(01.Triangle CompileShaders)
# ------------------------------------------------------------------
# 01.Triangle.Subset —— 可移植子集（三角形 + 旋转 + ImGui）
#
# 与鸿蒙入口共用 Samples/01.Triangle/App/01.Triangle.App.cpp：
#   · Windows：Main_Subset.cpp
#   · 鸿蒙：   Platforms/OHOS/entry/src/main/cpp/AppMain.cpp
# 用于在桌面快速回归"鸿蒙将要跑的那份渲染逻辑"。
# ------------------------------------------------------------------
if(NOT HUGENGINE_PLATFORM_OHOS)
    add_executable(01.Triangle.Subset
        Main_Subset.cpp
        App/01.Triangle.App.cpp
        App/01.Triangle.App.h
    )
    target_link_libraries(01.Triangle.Subset PRIVATE
        HugEngineCore
        HugEngineRHI
        HugEngineShader
        HugEngineEditor
    )
    target_include_directories(01.Triangle.Subset PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/App)
    add_dependencies(01.Triangle.Subset CompileShaders)
endif()