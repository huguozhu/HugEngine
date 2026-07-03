add_executable(04.Deferred main.cpp)
target_link_libraries(04.Deferred PRIVATE HugEngineCore HugEngineRHI HugEngineShader HugEngineScene HugEngineRender HugEngineEditor stb)
target_compile_definitions(04.Deferred PRIVATE HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/")
add_dependencies(04.Deferred CompileShaders)
