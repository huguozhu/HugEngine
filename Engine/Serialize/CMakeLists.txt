# Serialize — 基于反射的序列化系统
# 仅依赖 ReflectionAPI，与宏后端解耦
add_library(HugEngineSerialize STATIC)

target_sources(HugEngineSerialize PRIVATE
    Private/BinaryArchive.cpp
)

target_include_directories(HugEngineSerialize
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/Public
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Private
)

target_link_libraries(HugEngineSerialize PUBLIC
    HugEngineReflect
)
