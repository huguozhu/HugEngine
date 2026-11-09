# 03.Sponza-Forward — 加载 Sponza glTF 场景，自由相机漫游
add_executable(03.Sponza-Forward 03.Sponza-Forward.cpp)

target_link_libraries(03.Sponza-Forward PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineScene
    HugEngineRender
    HugEngineEditor
    HugEngineAsset
    stb   # 纹理解码 (stb_image.h)
)

# 定义 Content 目录绝对路径（解决从不同工作目录运行的路径问题）
target_compile_definitions(03.Sponza-Forward PRIVATE
    HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
)

# 依赖 CompileShaders 确保 EmbeddedShaders.h 已生成
# 样例共用头文件（相机输入等），见 Samples/Common/
target_include_directories(03.Sponza-Forward PRIVATE ${CMAKE_SOURCE_DIR}/Samples/Common)
add_dependencies(03.Sponza-Forward CompileShaders)

# ------------------------------------------------------------------
# 03.Sponza.Subset —— 可移植子集（Sponza + 前向管线 + 天空盒 + 相机）
#
# 与鸿蒙入口共用 Samples/03.Sponza-Forward/App/03.Sponza.App.cpp；
# 完整功能（RT / 配置 / 整套面板）仍在 03.Sponza-Forward。
# ------------------------------------------------------------------
if(NOT HUGENGINE_PLATFORM_OHOS)
    add_executable(03.Sponza.Subset
        Main_Subset.cpp
        App/03.Sponza.App.cpp
        App/03.Sponza.App.h
    )
    target_link_libraries(03.Sponza.Subset PRIVATE
        HugEngineCore
        HugEngineRHI
        HugEngineShader
        HugEngineScene
        HugEngineRender
        HugEngineEditor
        HugEngineAsset
        stb
    )
    target_include_directories(03.Sponza.Subset PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/App
        ${CMAKE_SOURCE_DIR}/Samples/Common)
    target_compile_definitions(03.Sponza.Subset PRIVATE
        HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
    )
    add_dependencies(03.Sponza.Subset CompileShaders)
endif()