# Scene — 场景管理，Actor-Component 架构 (L5)
# Phase 1 · Weeks 16-17

add_library(HugEngineScene STATIC)

set(SCENE_SOURCES
    Public/Scene/Entity.h
    Public/Scene/Component.h
    Public/Scene/Transform.h
    Public/Scene/World.h
    Public/Scene/SceneGraph.h
    Public/Scene/MeshComponent.h
    Public/Scene/LightComponent.h
    Public/Scene/SkyboxComponent.h
    Public/Scene/CameraComponent.h
    Public/Scene/CubeComponent.h
    Public/Scene/SphereComponent.h
    Public/Scene/AnimationComponent.h
    Public/Scene/LevelComponent.h
    Private/World.cpp
    Private/SceneGraph.cpp
    Private/MeshComponent.cpp
    Private/SceneReflect.cpp
    Private/CubeComponent.cpp
    Private/SphereComponent.cpp
    Private/LightComponent.cpp
    Private/SkyboxComponent.cpp
    Private/AnimationComponent.cpp
)

target_sources(HugEngineScene PRIVATE ${SCENE_SOURCES})

# IDE 目录树：按磁盘目录分组，.h 和 .cpp 合并在一起
huge_source_group(${SCENE_SOURCES})

target_include_directories(HugEngineScene
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/Public
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Private
)

target_link_libraries(HugEngineScene PUBLIC
    HugEngineReflect
    HugEngineCore
    HugEngineRHI
)
