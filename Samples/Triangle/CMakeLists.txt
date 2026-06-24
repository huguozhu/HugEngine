# Triangle — 使用 HugEngine RHI 渲染三角形
add_executable(Triangle main.cpp)

target_link_libraries(Triangle PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
)

# 依赖 CompileShaders 确保 EmbeddedShaders.h 已生成
add_dependencies(Triangle CompileShaders)