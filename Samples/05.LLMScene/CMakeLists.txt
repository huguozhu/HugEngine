# 05.LLMScene — LLM 一句话生成场景演示（DeepSeek）
add_executable(05.LLMScene 05.LLMScene.cpp)

target_link_libraries(05.LLMScene PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineScene
    HugEngineRender
    HugEngineEditor
    HugEngineAI
    stb
)

target_compile_definitions(05.LLMScene PRIVATE
    HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
)

# 依赖 CompileShaders 确保 EmbeddedShaders.h 已生成
add_dependencies(05.LLMScene CompileShaders)
