# 02.Cube — PBR 前向渲染管线演示（立方体+球体）
add_executable(02.Cube 02.Cube.cpp
    # P6：瞬态纹理 / 内存别名自检（HE_TRANSIENT_ALIAS_TEST=1 时运行，D3D12 与 Vulkan 共用）
    ${CMAKE_SOURCE_DIR}/Samples/Common/TransientAliasTest.cpp
)

target_link_libraries(02.Cube PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineScene
    HugEngineRender
    HugEnginePhysics
    HugEngineEditor
    stb   # stb_image.h 纹理解码
)

target_compile_definitions(02.Cube PRIVATE
    HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
)

# 依赖 CompileShaders 确保 EmbeddedShaders.h 已生成
# 样例共用头文件（相机输入等），见 Samples/Common/
target_include_directories(02.Cube PRIVATE ${CMAKE_SOURCE_DIR}/Samples/Common)
add_dependencies(02.Cube CompileShaders)

# ------------------------------------------------------------------
# 02.Cube.Subset —— 可移植子集（立方体 + 球体 + 天空盒 + 相机）
#
# 与鸿蒙入口共用 Samples/02.Cube/App/02.Cube.App.cpp：
#   · Windows：本文件（Main_Subset.cpp）
#   · 鸿蒙：  Platforms/OHOS/entry/src/main/cpp/AppMain.cpp
# 平时用这个 target 回归"鸿蒙将要跑的那份渲染逻辑"。
# ------------------------------------------------------------------
if(NOT HUGENGINE_PLATFORM_OHOS)
    add_executable(02.Cube.Subset
        Main_Subset.cpp
        App/02.Cube.App.cpp
        App/02.Cube.App.h
    )
    target_link_libraries(02.Cube.Subset PRIVATE
        HugEngineCore
        HugEngineRHI
        HugEngineShader
        HugEngineScene
        HugEngineRender
        HugEngineEditor
        stb
    )
    target_include_directories(02.Cube.Subset PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/App
        ${CMAKE_SOURCE_DIR}/Samples/Common)
    add_dependencies(02.Cube.Subset CompileShaders)
endif()