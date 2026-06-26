# HugEditor — 独立编辑器应用
add_executable(HugEditor
    main.cpp
    EditorApp.cpp
    Panels/ViewportPanel.cpp
    Panels/OutlinerPanel.cpp
    Panels/DetailsPanel.cpp
    Panels/ContentBrowserPanel.cpp
    Panels/ProjectSettingsPanel.cpp
)

target_include_directories(HugEditor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# 定义 Content 目录绝对路径（解决从不同工作目录运行的路径问题）
target_compile_definitions(HugEditor PRIVATE
    HUGE_CONTENT_DIR="${CMAKE_SOURCE_DIR}/Content/"
)

target_link_libraries(HugEditor PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineRender
    HugEngineScene
    HugEngineEditor
    HugEngineAsset
)

add_dependencies(HugEditor CompileShaders)
