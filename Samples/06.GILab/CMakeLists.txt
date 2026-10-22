# CrashHandler 已下沉到 HugEngineCore（Core/CrashHandler.cpp）：示例与单元测试共用同一份
# 实现，两边的崩溃报告不再各自为政（见 Core/CrashHandler.h 的说明）。
add_executable(06.GILab 06.GILab.cpp)
target_link_libraries(06.GILab PRIVATE HugEngineCore HugEngineRHI HugEngineShader HugEngineScene HugEngineRender HugEngineEditor HugEngineAsset stb)
target_compile_definitions(06.GILab PRIVATE HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/")
# 样例共用头文件（相机输入等），见 Samples/Common/
target_include_directories(06.GILab PRIVATE ${CMAKE_SOURCE_DIR}/Samples/Common)
add_dependencies(06.GILab CompileShaders)
