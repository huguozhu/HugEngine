# Editor — Editor framework (Phase 1)
add_library(HugEngineEditor STATIC)

set(EDITOR_SOURCES
    Public/Editor/CVar.h
    Public/Editor/Command.h
    Public/Editor/ImGuiIntegration.h
    Public/Editor/EditorContext.h
    Public/Editor/SceneSerializer.h
    Private/CVar.cpp
    Private/Command.cpp
    Private/ImGuiIntegration.cpp
    Private/EditorContext.cpp
    Private/SceneSerializer.cpp
)

target_sources(HugEngineEditor PRIVATE ${EDITOR_SOURCES})

# IDE 目录树：按磁盘目录分组，.h 和 .cpp 合并在一起
huge_source_group(${EDITOR_SOURCES})

target_include_directories(HugEngineEditor
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/Public
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Private
    PRIVATE ${CMAKE_SOURCE_DIR}/Engine/RHI/Vulkan  # VulkanInternal.h
)

target_link_libraries(HugEngineEditor PUBLIC HugEngineRender HugEngineSerialize imgui)
