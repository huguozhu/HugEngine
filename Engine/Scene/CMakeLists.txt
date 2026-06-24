# Scene — Entity-Component, SceneGraph (L5)
# Phase 1 · Weeks 16-17 (stub)

add_library(HugEngineScene INTERFACE)

target_include_directories(HugEngineScene INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Public)

target_link_libraries(HugEngineScene INTERFACE HugEngineReflect HugEngineCore)
