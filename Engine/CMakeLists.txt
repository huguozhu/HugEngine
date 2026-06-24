# ============================================================
# Engine sub-modules
# ============================================================

# Core — Platform, Math, Memory, Containers, Threading (L0)
add_subdirectory(Core)

# Reflect — C++26 static reflection system (L1)
add_subdirectory(Reflect)

# RHI — Rendering Hardware Interface (L2)
add_subdirectory(RHI)

# Shader — Slang compiler + hot reload (L3)
add_subdirectory(Shader)

# Scene — Entity-Component, SceneGraph (L5)
add_subdirectory(Scene)

# Asset — Resource loading, glTF, registry
add_subdirectory(Asset)

# Render — RenderGraph, pipelines, lighting, post-process (L4)
add_subdirectory(Render)

# Editor — Editor framework (L8)
if(HUGENGINE_BUILD_EDITOR)
    add_subdirectory(Editor)
endif()
