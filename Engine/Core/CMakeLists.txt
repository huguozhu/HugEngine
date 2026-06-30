# Core — Platform abstraction, Math, Memory, Containers, Threading (L0)
# Phase 1

add_library(HugEngineCore STATIC)

target_sources(HugEngineCore PRIVATE
    Core/Engine.cpp
    Core/Log.cpp
    Math/Geometry.cpp
    Platform/Window.cpp
    Threading/JobSystem.cpp
)

target_include_directories(HugEngineCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(HugEngineCore PUBLIC
    glfw
    glm::glm
    spdlog::spdlog
    Taskflow
)

