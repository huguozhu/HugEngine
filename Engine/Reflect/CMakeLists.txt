# Reflect — 宏驱动反射系统 (L1)
# Phase 1 · Weeks 3-4

add_library(HugEngineReflect STATIC)

set(REFLECT_SOURCES
    Public/Reflect/ReflectionAPI.h
    Public/Reflect/ReflectionMacros.h
    Public/Reflect/TypeInfo.h
    Public/Reflect/Attribute.h
    Private/TypeRegistry.cpp
)

target_sources(HugEngineReflect PRIVATE ${REFLECT_SOURCES})

# IDE 目录树：按磁盘目录分组，.h 和 .cpp 合并在一起
huge_source_group(${REFLECT_SOURCES})

target_include_directories(HugEngineReflect
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/Public
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Private
)

target_link_libraries(HugEngineReflect PUBLIC HugEngineCore)
