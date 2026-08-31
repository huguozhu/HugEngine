# 09.MaterialGen — 文生材质 + GPU 纹理生成（G2.2 本地资产生成验证）
add_executable(09.MaterialGen 09.MaterialGen.cpp)

target_link_libraries(09.MaterialGen PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineScene
    HugEngineRender
    HugEngineEditor
    HugEngineAI
    stb
)

target_compile_definitions(09.MaterialGen PRIVATE
    HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
)

# 依赖 CompileShaders 确保 EmbeddedShaders.h 已生成
add_dependencies(09.MaterialGen CompileShaders)
