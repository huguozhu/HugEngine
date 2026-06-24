# Reflect — 宏驱动反射系统 (L1)
# Phase 1 · Weeks 3-4

add_library(HugEngineReflect STATIC)

target_sources(HugEngineReflect PRIVATE
    Private/TypeRegistry.cpp
)

target_include_directories(HugEngineReflect
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/Public
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Private
)

target_link_libraries(HugEngineReflect PUBLIC HugEngineCore)
