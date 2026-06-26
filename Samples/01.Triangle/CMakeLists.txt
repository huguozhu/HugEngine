# 01.Triangle — 使用 HugEngine RHI 渲染三角形
add_executable(01.Triangle main.cpp)

target_link_libraries(01.Triangle PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
)

# 依赖 CompileShaders 确保 EmbeddedShaders.h 已生成
add_dependencies(01.Triangle CompileShaders)