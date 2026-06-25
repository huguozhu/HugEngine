# HugEditor — 独立编辑器应用
add_executable(HugEditor
    main.cpp
    EditorApp.cpp
    Panels/ViewportPanel.cpp
    Panels/OutlinerPanel.cpp
    Panels/DetailsPanel.cpp
)

target_include_directories(HugEditor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(HugEditor PRIVATE
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineRender
    HugEngineScene
    HugEngineEditor
)

add_dependencies(HugEditor CompileShaders)
