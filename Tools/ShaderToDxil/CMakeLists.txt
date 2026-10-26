# ShaderToDxil：把 slangc 产出的 HLSL 补丁后交给 dxc 生成 DXIL（D3D12 后端用）
add_executable(ShaderToDxil main.cpp)
set_target_properties(ShaderToDxil PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
