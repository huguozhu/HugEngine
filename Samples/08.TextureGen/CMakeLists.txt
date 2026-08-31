# 08.TextureGen — 文生纹理（G2.1 资产生成验证）
add_executable(08.TextureGen 08.TextureGen.cpp)

target_link_libraries(08.TextureGen PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineScene
    HugEngineRender
    HugEngineEditor
    HugEngineAI
    stb
)

target_compile_definitions(08.TextureGen PRIVATE
    HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
)

# 依赖 CompileShaders 确保 EmbeddedShaders.h 已生成
add_dependencies(08.TextureGen CompileShaders)
