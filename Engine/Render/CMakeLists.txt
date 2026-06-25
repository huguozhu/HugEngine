# Render — RenderGraph, Pipelines, Lighting, PostProcess (L4)
# Phase 1

add_library(HugEngineRender STATIC)

target_sources(HugEngineRender PRIVATE
    Private/RenderGraph.cpp
    Private/Pipeline/ForwardPipeline.cpp
)

target_include_directories(HugEngineRender
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/Public
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Private
)

target_link_libraries(HugEngineRender PUBLIC HugEngineRHI HugEngineShader HugEngineScene)
