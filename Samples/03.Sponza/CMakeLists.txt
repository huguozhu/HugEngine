# 03.Sponza — 加载 Sponza glTF 场景，自由相机漫游
add_executable(03.Sponza main.cpp)

target_link_libraries(03.Sponza PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineScene
    HugEngineRender
    HugEngineEditor
    HugEngineAsset
)

# 定义 Content 目录绝对路径（解决从不同工作目录运行的路径问题）
target_compile_definitions(03.Sponza PRIVATE
    HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
)

# 依赖 CompileShaders 确保 EmbeddedShaders.h 已生成
add_dependencies(03.Sponza CompileShaders)
