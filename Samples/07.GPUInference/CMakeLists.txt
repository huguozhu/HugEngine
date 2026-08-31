# 07.GPUInference — GPU 推理后端验证（张量 → compute 核 → 读回）
add_executable(07.GPUInference 07.GPUInference.cpp)

target_link_libraries(07.GPUInference PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineScene
    HugEngineRender
    HugEngineEditor
    HugEngineAI
    stb
)

target_compile_definitions(07.GPUInference PRIVATE
    HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
)

# 依赖 CompileShaders 确保 EmbeddedShaders.h（含 AITensorScale）已生成
add_dependencies(07.GPUInference CompileShaders)
