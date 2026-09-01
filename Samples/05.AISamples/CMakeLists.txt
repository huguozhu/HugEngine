# 05.AISamples — AI 综合示例（原 05~10 六个 AI Sample 合并）
add_executable(05.AISamples
    AISamples.cpp
    Features/IFeature.h
    Features/FeatureLLMScene.cpp
    Features/FeatureLLMScene.h
    Features/FeatureAgentScene.cpp
    Features/FeatureAgentScene.h
    Features/FeatureGPUInference.cpp
    Features/FeatureGPUInference.h
    Features/FeatureTextureGen.cpp
    Features/FeatureTextureGen.h
    Features/FeatureMaterialGen.cpp
    Features/FeatureMaterialGen.h
    Features/FeatureMeshAnimGen.cpp
    Features/FeatureMeshAnimGen.h
)

target_include_directories(05.AISamples PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(05.AISamples PRIVATE
    HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
)

target_link_libraries(05.AISamples PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineScene
    HugEngineRender
    HugEngineEditor
    HugEngineAI
    stb
)

# 依赖 CompileShaders 确保 EmbeddedShaders.h 已生成
add_dependencies(05.AISamples CompileShaders)
