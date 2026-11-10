# 03.Sponza-Forward — 加载 Sponza glTF 场景，自由相机漫游
add_executable(03.Sponza-Forward 03.Sponza-Forward.cpp)

target_link_libraries(03.Sponza-Forward PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineScene
    HugEngineRender
    HugEngineEditor
    HugEngineAsset
    stb   # 纹理解码 (stb_image.h)
)

# 定义 Content 目录绝对路径（解决从不同工作目录运行的路径问题）
target_compile_definitions(03.Sponza-Forward PRIVATE
    HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
)

# 依赖 CompileShaders 确保 EmbeddedShaders.h 已生成
# 样例共用头文件（相机输入等），见 Samples/Common/
target_include_directories(03.Sponza-Forward PRIVATE ${CMAKE_SOURCE_DIR}/Samples/Common)
add_dependencies(03.Sponza-Forward CompileShaders)


# ============================================================
# 路线1 迁移：可移植 App（桌面/鸿蒙共用）
#
# 桌面入口在 03.Sponza.App.cpp 的 HE_DESKTOP_ENTRY 段里；鸿蒙 entry 编同一份 .cpp。
# 完整版 03.Sponza-Forward 暂时保留，等 RT/配置/渲染线程并入 App 后删除。
# ============================================================
add_executable(03.Sponza.App
    03.Sponza.App.cpp
    03.Sponza.App.h
    ${CMAKE_SOURCE_DIR}/Samples/Common/SkyboxLoader.cpp
    ${CMAKE_SOURCE_DIR}/Samples/Common/SkyboxLoader.h
)
target_compile_definitions(03.Sponza.App PRIVATE
    HE_DESKTOP_ENTRY=1
    HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
)
target_include_directories(03.Sponza.App PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Samples/Common)
target_link_libraries(03.Sponza.App PRIVATE
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
add_dependencies(03.Sponza.App CompileShaders)
