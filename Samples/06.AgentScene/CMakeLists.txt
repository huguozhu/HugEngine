# 06.AgentScene — AI 智能体演示（AgentSystem + MockBrain）
add_executable(06.AgentScene 06.AgentScene.cpp)

target_link_libraries(06.AgentScene PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineScene
    HugEngineRender
    HugEngineEditor
    HugEngineAI
    stb
)

target_compile_definitions(06.AgentScene PRIVATE
    HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
)

# 依赖 CompileShaders 确保 EmbeddedShaders.h 已生成
add_dependencies(06.AgentScene CompileShaders)
