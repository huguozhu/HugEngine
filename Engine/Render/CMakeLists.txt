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
    Pipeline/PhysicalCamera.h
    Pipeline/PhysicalCamera.cpp
    Pipeline/PhysicalLight.h
    Pipeline/ForwardPipeline.h
    Pipeline/ForwardPipeline.cpp
    Pipeline/ForwardPipeline_FrameGraph.cpp
    Pipeline/ParticleRenderer.h
    Pipeline/ParticleRenderer.cpp
    Pipeline/DeferredPipeline.h
    Pipeline/DeferredPipeline.cpp
    Pipeline/DeferredPipeline_FrameGraph.cpp
    Pipeline/RTQualityCVars.h
    Pipeline/RTQualityCVars.cpp
    Pipeline/PathTracingPipeline.h
    Pipeline/PathTracingPipeline.cpp
    Pipeline/PTQualityCVars.h
    Pipeline/PTQualityCVars.cpp
    Pipeline/ClusteredShading.h
    Pipeline/ClusteredShading.cpp
    Pipeline/GPUScene.h
    Pipeline/GPUScene.cpp
    Pipeline/MeshBatcher.h
    Pipeline/MeshBatcher.cpp
    Pipeline/GPUCulling.h
    Pipeline/GPUCulling.cpp
    Pipeline/GBufferRenderer.h
    Pipeline/GBufferRenderer.cpp
    Pipeline/LightingPass.h
    Pipeline/LightingPass.cpp
    Pipeline/GBufferRenderer_CPU.h
    Pipeline/GBufferRenderer_CPU.cpp
    Pipeline/GBufferRenderer_GPU.h
    Pipeline/GBufferRenderer_GPU.cpp
    Pipeline/DecalPass.h
    Pipeline/DecalPass.cpp
    Pipeline/InstanceCuller.h
    Pipeline/InstanceCuller.cpp
    Pipeline/GPUWorkGraph.h
    Pipeline/GPUWorkGraph.cpp
    Pipeline/Material.h
    Pipeline/RTPass.h
    Pipeline/RTPass.cpp
    # RT 效果 Pass（HybridRT 管线）
    RT/RTEffectPass.h
    RT/RTEffectPass.cpp
    RT/RTShadowPass.h
    RT/RTShadowPass.cpp
    RT/RTAOPass.h
    RT/RTAOPass.cpp
    RT/RTReflectionPass.h
    RT/RTReflectionPass.cpp
    RT/RTGIPass.h
    RT/RTGIPass.cpp
    RT/PTPass.h
    RT/PTPass.cpp
    RT/ReSTIRPass.h
    RT/ReSTIRPass.cpp
    RT/STBNTexture.h
    RT/STBNTexture.cpp
    # STBN 数据由 Tools/gen_stbn.py 离线生成（~20MB，列入仅为 IDE 浏览）
    RT/STBNData.h
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
    GI/GIRadianceHistory.h
    GI/GIRadianceHistory.cpp
    GI/GITiming.h
    GI/GITiming.cpp
    GI/GI_SSR.h
    GI/GI_SSR.cpp
    GI/GI_DDGI.h
    GI/GI_DDGI.cpp
    # RSM 间接光的半分辨率求值 pass（任务 16 / B3）
    GI/RSMIndirect.h
    GI/RSMIndirect.cpp
    # DDGI 探针射线的光追 march（任务 17 / B4）
    GI/DDGITracePass.h
    GI/DDGITracePass.cpp
    # GI — 数据模型（RHI-free，可被 Tests 直接包含）
    GI/GITypes.h
    # GI — 探针网格拟合规则（RHI-free，可被 Tests 直接包含）
    GI/GIProbeGrid.h
    # GI Provider（P4 / Wave 2：GI 源统一抽象 + 各源实现）
    GI/IGIProvider.h
    GI/IBLProvider.h
    GI/RSMProvider.h
    GI/AOProvider.h
    GI/SpatialDenoiseAux.h
    GI/SSGIProvider.h
    GI/SSRProvider.h
    GI/DDGIProvider.h
    GI/RTProvider.h
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
    Shadow/RectLightShadowTechnique.h
    Shadow/RectLightShadowTechnique.cpp
    # PostProcess
    PostProcess/PostProcessPass.h
    PostProcess/LazyPostProcessPass.h
    PostProcess/ToneMapPass.h
    PostProcess/ToneMapPass.cpp
    PostProcess/SkyboxPass.h
    PostProcess/SkyboxPass.cpp
    PostProcess/SSAO.h
    PostProcess/SSAO.cpp
    PostProcess/Denoiser.h
    PostProcess/Denoiser.cpp
    PostProcess/RTDenoiser.h
    PostProcess/RTDenoiser.cpp
    PostProcess/PTAtrousPass.h
    PostProcess/PTAtrousPass.cpp
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
    PostProcess/CameraEffectsPass.h
    PostProcess/CameraEffectsPass.cpp
    PostProcess/PostProcessChain.h
    PostProcess/PostProcessChain.cpp
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
    # Lumen（虚拟化几何 GI）：持久资源宿主 + Provider，后续步骤在其上追加 atlas / SDF / 探针
    Lumen/LumenScene.h
    Lumen/LumenScene.cpp
    GI/LumenProvider.h
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
