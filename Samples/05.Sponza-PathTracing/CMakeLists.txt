add_executable(05.Sponza-PathTracing 05.Sponza-PathTracing.cpp)
target_link_libraries(05.Sponza-PathTracing PRIVATE HugEngineCore HugEngineRHI HugEngineShader HugEngineScene HugEngineRender HugEngineEditor HugEngineAsset stb)
target_compile_definitions(05.Sponza-PathTracing PRIVATE HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/")
add_dependencies(05.Sponza-PathTracing CompileShaders)
