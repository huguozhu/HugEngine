# Render — RenderGraph, Pipelines, Lighting, PostProcess, GI (L4)
# 头文件列入 target_sources 方便 IDE 浏览，不参与编译
add_library(HugEngineRender STATIC)

set(RENDER_SOURCES
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
    Pipeline/ForwardPipeline_FrameGraph.cpp
    Pipeline/ParticleRenderer.h
    Pipeline/ParticleRenderer.cpp
    Pipeline/DeferredPipeline.h
    Pipeline/DeferredPipeline.cpp
    Pipeline/DeferredPipeline_FrameGraph.cpp
    Pipeline/ClusteredShading.h
    Pipeline/ClusteredShading.cpp
    Pipeline/GPUScene.h
    Pipeline/GPUScene.cpp
    Pipeline/MeshBatcher.h
    Pipeline/MeshBatcher.cpp
    Pipeline/GPUCulling.h
    Pipeline/GPUCulling.cpp
    Pipeline/GBufferRenderer.h
    Pipeline/GBufferRenderer_CPU.h
    Pipeline/GBufferRenderer_CPU.cpp
    Pipeline/GBufferRenderer_GPU.h
    Pipeline/GBufferRenderer_GPU.cpp
    Pipeline/GPUWorkGraph.h
    Pipeline/GPUWorkGraph.cpp
    Pipeline/Material.h
    Pipeline/RTPass.h
    Pipeline/RTPass.cpp
    # RenderGraph
    RenderGraph.h
    RenderGraph.cpp
    # Subsystem
    Subsystem/RenderSubsystem.h
    # GI
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
    # Shadow
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
    PostProcess/SSAO.h
    PostProcess/SSAO.cpp
    PostProcess/Denoiser.h
    PostProcess/Denoiser.cpp
    PostProcess/GaussianBlurPass.h
    PostProcess/GaussianBlurPass.cpp
    PostProcess/BloomPass.h
    PostProcess/BloomPass.cpp
    PostProcess/DOFPass.h
    PostProcess/DOFPass.cpp
    PostProcess/MotionBlurPass.h
    PostProcess/MotionBlurPass.cpp
    PostProcess/AutoExposurePass.h
    PostProcess/AutoExposurePass.cpp
    PostProcess/ColorGradingPass.h
    PostProcess/ColorGradingPass.cpp
    # Profiler
    Profiler/ProfilerManager.h
    Profiler/ProfilerManager.cpp
    Profiler/ProfilerPanel.h
    Profiler/ProfilerPanel.cpp
    # AntiAliasing
    AntiAliasing/AntiAliasing.h
    AntiAliasing/AA_None.h
    AntiAliasing/AA_TAA.h
    AntiAliasing/AA_TAA.cpp
    AntiAliasing/AA_FXAA.h
    AntiAliasing/AA_FXAA.cpp
    AntiAliasing/AA_SMAA.h
    AntiAliasing/AA_SMAA.cpp
    AntiAliasing/AA_MSAA.h
    # ShaderHotReload
    ShaderHotReload.h
    ShaderHotReload.cpp
)

target_sources(HugEngineRender PRIVATE ${RENDER_SOURCES})

# IDE 目录树：按磁盘目录分组，.h 和 .cpp 合并在一起
huge_source_group(${RENDER_SOURCES})

target_include_directories(HugEngineRender
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

# ImGui 头文件路径（供 ProfilerPanel 使用）
target_include_directories(HugEngineRender PRIVATE ${CMAKE_SOURCE_DIR}/Engine/External/imgui)
target_link_libraries(HugEngineRender PUBLIC HugEngineRHI HugEngineShader HugEngineScene HugEngineAsset)
