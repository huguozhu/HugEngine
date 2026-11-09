add_executable(04.Sponza-Deferred 04.Sponza-Deferred.cpp)
target_link_libraries(04.Sponza-Deferred PRIVATE HugEngineCore HugEngineRHI HugEngineShader HugEngineScene HugEngineRender HugEngineEditor HugEngineAsset stb)
target_compile_definitions(04.Sponza-Deferred PRIVATE HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/")
# 样例共用头文件（相机输入等），见 Samples/Common/
target_include_directories(04.Sponza-Deferred PRIVATE ${CMAKE_SOURCE_DIR}/Samples/Common)
add_dependencies(04.Sponza-Deferred CompileShaders)
