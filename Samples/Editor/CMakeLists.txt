# HugEditor — 独立编辑器应用
set(HUGEDITOR_SOURCES
    main.cpp
    EditorApp.cpp
    EditorApp.h
    Panels/ViewportPanel.cpp
    Panels/ViewportPanel.h
    Panels/OutlinerPanel.cpp
    Panels/OutlinerPanel.h
    Panels/DetailsPanel.cpp
    Panels/DetailsPanel.h
    Panels/ContentBrowserPanel.cpp
    Panels/ContentBrowserPanel.h
    Panels/ProjectSettingsPanel.cpp
    Panels/ProjectSettingsPanel.h
    Panels/StatsPanel.cpp
    Panels/StatsPanel.h
    Panels/Gizmo.cpp
    Panels/Gizmo.h
    Panels/LevelLoader.cpp
    Panels/LevelLoader.h
    Panels/ConsolePanel.cpp
    Panels/ConsolePanel.h
    Panels/MaterialEditor.cpp
    Panels/MaterialEditor.h
)

add_executable(HugEditor ${HUGEDITOR_SOURCES})

# IDE 目录树：按磁盘目录分组，.h 和 .cpp 合并在一起
huge_source_group(${HUGEDITOR_SOURCES})

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
    stb
)

add_dependencies(HugEditor CompileShaders)
