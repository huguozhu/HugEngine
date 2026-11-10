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


# ============================================================
# 路线1 迁移：可移植 App（桌面/鸿蒙共用）
# 桌面入口在 02.Cube.App.cpp 的 HE_DESKTOP_ENTRY 段里。
# 完整版 02.Cube 暂时保留，等 CVar/物理/面板并入 App 后删除。
# ============================================================
add_executable(02.Cube.App
    02.Cube.App.cpp
    02.Cube.App.h
    ${CMAKE_SOURCE_DIR}/Samples/Common/SkyboxLoader.cpp
    ${CMAKE_SOURCE_DIR}/Samples/Common/SkyboxLoader.h
)
target_compile_definitions(02.Cube.App PRIVATE
    HE_DESKTOP_ENTRY=1
    HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
)
target_include_directories(02.Cube.App PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Samples/Common)
target_link_libraries(02.Cube.App PRIVATE
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
add_dependencies(02.Cube.App CompileShaders)
