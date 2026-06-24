# Render — RenderGraph, Pipelines, Lighting, PostProcess (L4)
# Phase 1-6 (stub)

add_library(HugEngineRender INTERFACE)

target_include_directories(HugEngineRender INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Public)

target_link_libraries(HugEngineRender INTERFACE HugEngineRHI HugEngineShader HugEngineScene)
