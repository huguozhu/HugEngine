# 02.Cube — PBR 前向渲染管线演示（立方体+球体）
add_executable(02.Cube 02.Cube.cpp)

target_link_libraries(02.Cube PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineScene
    HugEngineRender
    HugEngineEditor
    stb   # stb_image.h 纹理解码
)

target_compile_definitions(02.Cube PRIVATE
    HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
)

# 依赖 CompileShaders 确保 EmbeddedShaders.h 已生成
add_dependencies(02.Cube CompileShaders)
