# 01.Triangle — 使用 HugEngine RHI 渲染三角形
add_executable(01.Triangle 01.Triangle.cpp)

target_link_libraries(01.Triangle PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineEditor
)

# 依赖 CompileShaders 确保 .spv.h 着色器头文件已生成
add_dependencies(01.Triangle CompileShaders)