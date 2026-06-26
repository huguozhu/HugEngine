# 02.Cube — PBR 前向渲染管线演示（立方体+球体）
add_executable(02.Cube main.cpp)

target_link_libraries(02.Cube PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineScene
    HugEngineRender
    HugEngineEditor
)

# 依赖 CompileShaders 确保 EmbeddedShaders.h 已生成
add_dependencies(02.Cube CompileShaders)
