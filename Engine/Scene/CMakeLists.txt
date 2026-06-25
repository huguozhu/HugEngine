# Scene — 场景管理，Actor-Component 架构 (L5)
# Phase 1 · Weeks 16-17

add_library(HugEngineScene STATIC)

target_sources(HugEngineScene PRIVATE
    Private/World.cpp
    Private/SceneGraph.cpp
    Private/MeshComponent.cpp
    Private/SceneReflect.cpp
    Private/CubeComponent.cpp
    Private/SphereComponent.cpp
    Private/LightComponent.cpp
)

target_include_directories(HugEngineScene
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/Public
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Private
)

target_link_libraries(HugEngineScene PUBLIC
    HugEngineReflect
    HugEngineCore
    HugEngineRHI
)
