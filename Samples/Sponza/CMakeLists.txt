# Sponza — PBR 前向渲染管线演示
add_executable(Sponza main.cpp)

target_link_libraries(Sponza PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineScene
    HugEngineRender
    HugEngineEditor
)

# 依赖 CompileShaders 确保 EmbeddedShaders.h 已生成
add_dependencies(Sponza CompileShaders)
