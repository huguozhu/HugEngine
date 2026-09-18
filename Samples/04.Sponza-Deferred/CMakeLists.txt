add_executable(04.Sponza-Deferred 04.Sponza-Deferred.cpp)
target_link_libraries(04.Sponza-Deferred PRIVATE HugEngineCore HugEngineRHI HugEngineShader HugEngineScene HugEngineRender HugEngineEditor HugEngineAsset stb)
target_compile_definitions(04.Sponza-Deferred PRIVATE HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/")
add_dependencies(04.Sponza-Deferred CompileShaders)
