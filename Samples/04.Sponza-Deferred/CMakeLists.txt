add_executable(04.Sponza-Deferred 04.Sponza-Deferred.cpp)
target_link_libraries(04.Sponza-Deferred PRIVATE HugEngineCore HugEngineRHI HugEngineShader HugEngineScene HugEngineRender HugEngineEditor HugEngineAsset stb)
target_compile_definitions(04.Sponza-Deferred PRIVATE HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/")
# 样例共用头文件（相机输入等），见 Samples/Common/
target_include_directories(04.Sponza-Deferred PRIVATE ${CMAKE_SOURCE_DIR}/Samples/Common)
add_dependencies(04.Sponza-Deferred CompileShaders)

# ------------------------------------------------------------------
# 04.Deferred.Subset —— 可移植子集（Sponza + 延迟管线 + 天空盒 + 相机）
#
# 与鸿蒙入口共用 Samples/04.Sponza-Deferred/App/04.Deferred.App.cpp；
# 完整功能（SSGI/DDGI/SSR/SSAO/粒子/配置）仍在 04.Sponza-Deferred。
# ------------------------------------------------------------------
if(NOT HUGENGINE_PLATFORM_OHOS)
    add_executable(04.Deferred.Subset
        Main_Subset.cpp
        App/04.Deferred.App.cpp
        App/04.Deferred.App.h
    )
    target_link_libraries(04.Deferred.Subset PRIVATE
        HugEngineCore
        HugEngineRHI
        HugEngineShader
        HugEngineScene
        HugEngineRender
        HugEngineEditor
        HugEngineAsset
        stb
    )
    target_include_directories(04.Deferred.Subset PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/App
        ${CMAKE_SOURCE_DIR}/Samples/Common)
    target_compile_definitions(04.Deferred.Subset PRIVATE
        HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
    )
    add_dependencies(04.Deferred.Subset CompileShaders)
endif()