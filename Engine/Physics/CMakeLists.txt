# ============================================================
# Engine/Physics/CMakeLists.txt — Jolt 物理模块（C2 最小竖切）
#
# 静态库 HugEnginePhysics（依赖 HugEngineScene / HugEngineCore / Jolt）
# 组件放本模块（与 AI 模块的 Agent 组件同模式），保证 Scene 层不依赖 Jolt 头文件。
# ============================================================

set(PHYSICS_SOURCES
    Physics/PhysicsWorld.h
    Physics/PhysicsWorld.cpp
    Physics/JoltConversions.h
    Physics/RigidBodyComponent.h
    Physics/RigidBodyComponent.cpp
    Physics/PhysicsSystem.h
    Physics/PhysicsSystem.cpp
    Physics/PhysicsReflect.cpp
)

add_library(HugEnginePhysics STATIC
    ${PHYSICS_SOURCES}
)

set_target_properties(HugEnginePhysics PROPERTIES
    FOLDER "Engine"   # 与其它 Engine 模块同一个解决方案文件夹（根 CMakeLists 也会统一设置）
)

target_include_directories(HugEnginePhysics
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Engine
)

target_link_libraries(HugEnginePhysics
    PUBLIC
        HugEngineScene
        HugEngineCore
        Jolt
)
