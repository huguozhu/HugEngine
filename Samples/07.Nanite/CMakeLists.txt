# CrashHandler 已下沉到 HugEngineCore（Core/CrashHandler.cpp）：示例与单元测试共用同一份
# 实现，两边的崩溃报告不再各自为政（见 Core/CrashHandler.h 的说明）。
add_executable(07.Nanite 07.Nanite.cpp)
target_link_libraries(07.Nanite PRIVATE HugEngineCore HugEngineRHI HugEngineShader HugEngineScene HugEngineRender HugEngineEditor HugEngineAsset stb)
target_compile_definitions(07.Nanite PRIVATE HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/")
add_dependencies(07.Nanite CompileShaders)
