# 10.MeshAnimGen — 文生网格 + 文生动画（G2.3 资产生成验证）
add_executable(10.MeshAnimGen 10.MeshAnimGen.cpp)

target_link_libraries(10.MeshAnimGen PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineScene
    HugEngineRender
    HugEngineEditor
    HugEngineAI
    stb
)

target_compile_definitions(10.MeshAnimGen PRIVATE
    HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
)

# 依赖 CompileShaders 确保 EmbeddedShaders.h 已生成
add_dependencies(10.MeshAnimGen CompileShaders)
