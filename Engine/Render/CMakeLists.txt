# Render — RenderGraph, Pipelines, Lighting, PostProcess, GI (L4)
# 头文件列入 target_sources 方便 IDE 浏览，不参与编译
add_library(HugEngineRender STATIC)

target_sources(HugEngineRender PRIVATE
    # SceneRenderer
    SceneRenderer.h
    SceneRenderer.cpp
    # Pipeline
    Pipeline/IRenderPipeline.h
    Pipeline/Camera.h
    Pipeline/CameraController.h
    Pipeline/CameraController.cpp
    Pipeline/Camera.cpp
    Pipeline/ForwardPipeline.h
    Pipeline/ForwardPipeline.cpp
    Pipeline/DeferredPipeline.h
    Pipeline/DeferredPipeline.cpp
    Pipeline/ClusteredShading.cpp
    Pipeline/GPUCulling.cpp
    Pipeline/Material.h
    # RenderGraph
    RenderGraph.h
    RenderGraph.cpp
    # Subsystem (基础接口)
    Subsystem/RenderSubsystem.h
    # GI (全局光照)
    GI/GlobalIllumination.h
    GI/GI_None.h
    GI/GI_None.cpp
    GI/GI_IBL.h
    GI/GI_IBL.cpp
    GI/GI_RSM.h
    GI/GI_RSM.cpp
    # Shadow (阴影子系统)
    Shadow/IShadowSystem.h
    Shadow/IShadowTechnique.h
    Shadow/ShadowNone.h
    Shadow/ShadowSystem.h
    Shadow/ShadowSystem.cpp
    Shadow/CSMTechnique.h
    Shadow/CSMTechnique.cpp
    Shadow/PointShadowTechnique.h
    Shadow/PointShadowTechnique.cpp
    Shadow/SpotShadowTechnique.h
    Shadow/SpotShadowTechnique.cpp
    # PostProcess
    PostProcess/PostProcessPass.h
    PostProcess/ToneMapPass.h
    PostProcess/ToneMapPass.cpp
    PostProcess/SkyboxPass.h
    PostProcess/SkyboxPass.cpp
    # AntiAliasing
    AntiAliasing/AntiAliasing.h
    AntiAliasing/AA_None.h
    AntiAliasing/AA_TAA.h
    AntiAliasing/AA_TAA.cpp
    AntiAliasing/AA_FXAA.h
    AntiAliasing/AA_FXAA.cpp
)

target_include_directories(HugEngineRender
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(HugEngineRender PUBLIC HugEngineRHI HugEngineShader HugEngineScene HugEngineAsset)
