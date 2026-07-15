# Editor — Editor framework (Phase 1)
add_library(HugEngineEditor STATIC)

set(EDITOR_SOURCES
    Editor/CVar.h
    Editor/Command.h
    Editor/ImGuiIntegration.h
    Editor/EditorContext.h
    Editor/SceneSerializer.h
    Editor/CVar.cpp
    Editor/Command.cpp
    Editor/ImGuiIntegration.cpp
    Editor/EditorContext.cpp
    Editor/SceneSerializer.cpp
)

target_sources(HugEngineEditor PRIVATE ${EDITOR_SOURCES})

# IDE 目录树：按磁盘目录分组，.h 和 .cpp 合并在一起
huge_source_group(${EDITOR_SOURCES})

target_include_directories(HugEngineEditor
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_SOURCE_DIR}/Engine/RHI/Vulkan  # VulkanInternal.h
)

target_link_libraries(HugEngineEditor PUBLIC HugEngineRender HugEngineSerialize imgui)
