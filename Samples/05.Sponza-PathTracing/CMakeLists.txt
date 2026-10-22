add_executable(05.Sponza-PathTracing 05.Sponza-PathTracing.cpp)
target_link_libraries(05.Sponza-PathTracing PRIVATE HugEngineCore HugEngineRHI HugEngineShader HugEngineScene HugEngineRender HugEngineEditor HugEngineAsset stb)
target_compile_definitions(05.Sponza-PathTracing PRIVATE HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/")
# 样例共用头文件（相机输入等），见 Samples/Common/
target_include_directories(05.Sponza-PathTracing PRIVATE ${CMAKE_SOURCE_DIR}/Samples/Common)
add_dependencies(05.Sponza-PathTracing CompileShaders)
