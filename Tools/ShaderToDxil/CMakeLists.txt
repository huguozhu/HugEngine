# ShaderToDxil：把 slangc 产出的 HLSL 补丁后编译成 D3D 字节码
#   默认模式：dxc 生成 DXIL（D3D12 后端用）
#   --dxbc 模式：寄存器重新分配后生成 DXBC（D3D11 后端用）
# 补丁实现放在 ShaderPatch.cpp，工具本体与单元测试共用。
# RHI/DxbcBindTable.h（绑定表格式，无其它引擎依赖）由工具与 D3D11 后端共用。
add_executable(ShaderToDxil main.cpp ShaderPatch.h ShaderPatch.cpp)
target_include_directories(ShaderToDxil PRIVATE "${CMAKE_SOURCE_DIR}/Engine/RHI")
# --dxbc 模式进程内调用 D3DCompile（Windows SDK 的 d3dcompiler.lib / d3dcompiler_47.dll）
target_link_libraries(ShaderToDxil PRIVATE d3dcompiler)
set_target_properties(ShaderToDxil PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

# 单元测试：手写 HLSL 片段，检查 D3D11 补丁的输出文本与绑定表（直接运行，返回 0 为通过）
add_executable(ShaderToDxilTests ShaderPatchTests.cpp ShaderPatch.h ShaderPatch.cpp)
target_include_directories(ShaderToDxilTests PRIVATE "${CMAKE_SOURCE_DIR}/Engine/RHI")
set_target_properties(ShaderToDxilTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
