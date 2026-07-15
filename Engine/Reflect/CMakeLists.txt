# Reflect — 宏驱动反射系统 (L1)
# Phase 1 · Weeks 3-4

add_library(HugEngineReflect STATIC)

set(REFLECT_SOURCES
    Reflect/ReflectionAPI.h
    Reflect/ReflectionMacros.h
    Reflect/TypeInfo.h
    Reflect/Attribute.h
    Reflect/TypeRegistry.cpp
)

target_sources(HugEngineReflect PRIVATE ${REFLECT_SOURCES})

# IDE 目录树：按磁盘目录分组，.h 和 .cpp 合并在一起
huge_source_group(${REFLECT_SOURCES})

target_include_directories(HugEngineReflect
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(HugEngineReflect PUBLIC HugEngineCore)
