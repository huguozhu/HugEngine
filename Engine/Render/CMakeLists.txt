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
    Pipeline/GPUScene.cpp
    Pipeline/MeshBatcher.cpp
    Pipeline/GPUCulling.cpp
    Pipeline/GBufferRenderer_CPU.cpp
    Pipeline/GBufferRenderer_GPU.cpp
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
    GI/GI_SSGI.h
    GI/GI_SSGI.cpp
    GI/GI_SSR.h
    GI/GI_SSR.cpp
    GI/GI_DDGI.h
    GI/GI_DDGI.cpp
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
    PostProcess/SSAO.cpp
    PostProcess/Denoiser.cpp
    PostProcess/GaussianBlurPass.cpp
    PostProcess/BloomPass.cpp
    PostProcess/DOFPass.cpp
    PostProcess/MotionBlurPass.cpp
    PostProcess/AutoExposurePass.cpp
    # Profiler
    Profiler/ProfilerManager.cpp

    # AntiAliasing
    AntiAliasing/AntiAliasing.h
    AntiAliasing/AA_None.h
    AntiAliasing/AA_TAA.h
    AntiAliasing/AA_TAA.cpp
    AntiAliasing/AA_FXAA.h
    AntiAliasing/AA_FXAA.cpp
    # ShaderHotReload
    ShaderHotReload.h
    ShaderHotReload.cpp
)

target_include_directories(HugEngineRender
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(HugEngineRender PUBLIC HugEngineRHI HugEngineShader HugEngineScene HugEngineAsset)
