# Scene — 场景管理，Actor-Component 架构 (L5)
# Phase 1 · Weeks 16-17

add_library(HugEngineScene STATIC)

set(SCENE_SOURCES
    Scene/Entity.h
    Scene/Component.h
    Scene/Transform.h
    Scene/World.h
    Scene/SceneGraph.h
    Scene/MeshComponent.h
    Scene/LightComponent.h
    Scene/SkyboxComponent.h
    Scene/CameraComponent.h
    Scene/CubeComponent.h
    Scene/SphereComponent.h
    Scene/AnimationComponent.h
    Scene/LevelComponent.h
    Scene/ParticleComponent.h
    Scene/World.cpp
    Scene/SceneGraph.cpp
    Scene/MeshComponent.cpp
    Scene/SceneReflect.cpp
    Scene/CubeComponent.cpp
    Scene/SphereComponent.cpp
    Scene/LightComponent.cpp
    Scene/SkyboxComponent.cpp
    Scene/AnimationComponent.cpp
    Scene/ParticleComponent.cpp
)

target_sources(HugEngineScene PRIVATE ${SCENE_SOURCES})

# IDE 目录树：按磁盘目录分组，.h 和 .cpp 合并在一起
huge_source_group(${SCENE_SOURCES})

target_include_directories(HugEngineScene
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(HugEngineScene PUBLIC
    HugEngineReflect
    HugEngineCore
    HugEngineRHI
)
