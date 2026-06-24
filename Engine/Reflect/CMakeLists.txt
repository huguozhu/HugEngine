# Reflect — 宏驱动反射系统 (L1)
# Phase 1 · Weeks 3-4

add_library(HugEngineReflect INTERFACE)

target_include_directories(HugEngineReflect INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Public)

target_link_libraries(HugEngineReflect INTERFACE HugEngineCore)
