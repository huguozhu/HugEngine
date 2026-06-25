# Editor — Editor framework (Phase 1)
add_library(HugEngineEditor STATIC)

target_sources(HugEngineEditor PRIVATE
    Private/CVar.cpp
    Private/Command.cpp
    Private/ImGuiIntegration.cpp
)

target_include_directories(HugEngineEditor
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/Public
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Private
    PRIVATE ${CMAKE_SOURCE_DIR}/Engine/RHI/Vulkan  # VulkanInternal.h
)

target_link_libraries(HugEngineEditor PUBLIC HugEngineRender imgui)
