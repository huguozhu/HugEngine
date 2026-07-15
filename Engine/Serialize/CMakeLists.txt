# Serialize — 基于反射的序列化系统
# 仅依赖 ReflectionAPI，与宏后端解耦
add_library(HugEngineSerialize STATIC)

set(SERIALIZE_SOURCES
    Serialize/Archive.h
    Serialize/BinaryArchive.h
    Serialize/BinaryArchive.cpp
)

target_sources(HugEngineSerialize PRIVATE ${SERIALIZE_SOURCES})

# IDE 目录树：按磁盘目录分组，.h 和 .cpp 合并在一起
huge_source_group(${SERIALIZE_SOURCES})

target_include_directories(HugEngineSerialize
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(HugEngineSerialize PUBLIC
    HugEngineReflect
)
