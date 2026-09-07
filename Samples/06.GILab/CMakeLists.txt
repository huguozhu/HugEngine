add_executable(06.GILab 06.GILab.cpp)
target_link_libraries(06.GILab PRIVATE HugEngineCore HugEngineRHI HugEngineShader HugEngineScene HugEngineRender HugEngineEditor HugEngineAsset stb)
target_compile_definitions(06.GILab PRIVATE HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/")
add_dependencies(06.GILab CompileShaders)
