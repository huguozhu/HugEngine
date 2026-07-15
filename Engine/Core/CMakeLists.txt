# Core — Platform abstraction, Math, Memory, Containers, Threading (L0)
# Phase 1

add_library(HugEngineCore STATIC)

set(CORE_SOURCES
    Core/Engine.cpp
    Core/Engine.h
    Core/Log.cpp
    Core/Log.h
    Core/Assert.h
    Core/Core.h
    Core/Platform.h
    Core/Types.h
    Math/Geometry.cpp
    Math/Geometry.h
    Math/Math.h
    Containers/Array.h
    Memory/Allocator.h
    Platform/Window.cpp
    Platform/Window.h
    Threading/JobSystem.cpp
    Threading/JobSystem.h
)

target_sources(HugEngineCore PRIVATE ${CORE_SOURCES})

# IDE 目录树：按磁盘目录分组，.h 和 .cpp 合并在一起
huge_source_group(${CORE_SOURCES})

target_include_directories(HugEngineCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(HugEngineCore PUBLIC
    glfw
    glm::glm
    spdlog::spdlog
    Taskflow
)
