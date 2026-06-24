# HugEngine 架构设计与任务划分

> **目标**: 基于 [HugEngine_Technical_Plan.md](HugEngine_Technical_Plan.md) 中 340+ 项特性，给出可落地的模块化架构设计、任务依赖图和分阶段实施计划。
>
> **核心原则**: 每一阶段产出可运行/可演示的引擎 → 渐进式叠加能力 → 依赖关系单向无环
>
> **最后更新**: 2026-06-24

---

## 目录

1. [架构全景](#1-架构全景)
2. [模块目录与依赖](#2-模块目录与依赖)
3. [依赖拓扑图](#3-依赖拓扑图)
4. [任务划分（WBS）](#4-任务划分wbs)
5. [分阶段实施计划](#5-分阶段实施计划)
6. [核心接口设计](#6-核心接口设计)
7. [测试策略](#7-测试策略)
8. [风险与缓解](#8-风险与缓解)

---

## 1. 架构全景

### 1.1 分层架构

```
┌──────────────────────────────────────────────────────────────────┐
│  L8  编辑器层 (Editor Layer)                    [Phase 1-8 渐进]  │
│  ┌──────────┐ ┌───────────────┐ ┌──────────┐ ┌──────────────┐   │
│  │ World    │ │ Material      │ │ Visual   │ │ Terrain/     │   │
│  │ Editor   │ │ Editor        │ │ Script   │ │ Foliage      │   │
│  └──────────┘ └───────────────┘ └──────────┘ └──────────────┘   │
├──────────────────────────────────────────────────────────────────┤
│  L7  游戏逻辑层 (Game Logic)                   [Phase 6+ 可选]    │
│  ┌──────────┐ ┌───────────────┐ ┌──────────────────────────┐     │
│  │ C#/Lua   │ │ Blueprint VM  │ │ Gameplay Systems         │     │
│  │ Bindings │ │ (VisualScript)│ │ (Physics, Audio, AI...)  │     │
│  └──────────┘ └───────────────┘ └──────────────────────────┘     │
├──────────────────────────────────────────────────────────────────┤
│  L6  引擎系统层 (Engine Systems)               [Phase 1-6 渐进]   │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────────────┐    │
│  │ Render   │ │Animation │ │ Audio    │ │ Network          │    │
│  │ System   │ │ System   │ │ System   │ │ Replication      │    │
│  └──────────┘ └──────────┘ └──────────┘ └──────────────────┘    │
├──────────────────────────────────────────────────────────────────┤
│  L5  组件层 (Component Layer)                  [★ Phase 1 核心]   │
│  ┌──────┐ ┌─────────┐ ┌──────────┐ ┌────────┐ ┌──────────┐     │
│  │Entity│ │Component│ │SceneGraph│ │Transform│ │ Prefab   │     │
│  │      │ │Lifecycle│ │(层级变换) │ │(矩阵+Dirty│ │(模板+覆盖)│     │
│  └──────┘ └─────────┘ └──────────┘ └────────┘ └──────────┘     │
├──────────────────────────────────────────────────────────────────┤
│  L4  渲染层 (Rendering Layer)                  [Phase 1-7 渐进]   │
│  ┌─────────┐ ┌──────────┐ ┌────────┐ ┌──────┐ ┌──────────┐     │
│  │Forward/ │ │Deferred/ │ │Shadow  │ │GI/RT │ │Neural    │     │
│  │Forward+ │ │VisBuffer │ │Maps    │ │(Lumen │ │Rendering │     │
│  │Pipeline │ │Pipeline  │ │VSM/RT  │ │DDGI…) │ │(DLSS...) │     │
│  └─────────┘ └──────────┘ └────────┘ └──────┘ └──────────┘     │
│  ┌─────────┐ ┌──────────┐ ┌──────────────┐ ┌──────────────┐    │
│  │Virtual  │ │Virtual   │ │Atmosphere+   │ │Caustics+     │    │
│  │Geometry │ │Texture   │ │Volumetrics   │ │3DGS          │    │
│  │(Nanite) │ │(SVT)     │ │              │ │              │    │
│  └─────────┘ └──────────┘ └──────────────┘ └──────────────┘    │
│  ┌──────────────────────────────────────────────────────────┐    │
│  │              Render Graph (帧资源编排)                     │    │
│  │  Pass DAG │ Barrier Auto-Gen │ Resource Alias │ Async    │    │
│  └──────────────────────────────────────────────────────────┘    │
├──────────────────────────────────────────────────────────────────┤
│  L3  着色器层 (Shader Layer)                   [★ Phase 1 核心]   │
│  ┌────────────┐ ┌──────────────┐ ┌──────────┐ ┌────────────┐    │
│  │ Slang      │ │ SPIR-V/DXIL  │ │ Shader   │ │ Advanced   │    │
│  │ Compiler   │ │ /MSL Backend │ │ HotReload│ │ Shader Dlv │    │
│  └────────────┘ └──────────────┘ └──────────┘ └────────────┘    │
├──────────────────────────────────────────────────────────────────┤
│  L2  RHI 层 (Rendering Hardware Interface)    [★ Phase 1 核心]   │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌──────────┐      │
│  │ Vulkan │ │ D3D12  │ │ Metal  │ │ WebGPU │ │ PSO Cache│      │
│  │ 1.3+   │ │SM6.10+ │ │  3     │ │ (W3C)  │ │ Pipeline │      │
│  └────────┘ └────────┘ └────────┘ └────────┘ └──────────┘      │
├──────────────────────────────────────────────────────────────────┤
│  L1  反射层 (Reflection Layer)                [★ Phase 1 核心]   │
│  ┌────────────────────┐ ┌──────────────┐ ┌──────────────────┐   │
│  │ C++26 ^T + [:...:] │ │[[engine::]]  │ │ Serialization    │   │
│  │ Type Registry      │ │Attributes    │ │(Binary + JSON)   │   │
│  └────────────────────┘ └──────────────┘ └──────────────────┘   │
├──────────────────────────────────────────────────────────────────┤
│  L0  平台层 (Platform Layer)                  [★ Phase 1 核心]   │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌──────────┐ ┌──────────┐    │
│  │GLFW/   │ │Thread/ │ │Memory  │ │File I/O  │ │ GLM +    │    │
│  │SDL3    │ │Job Sys │ │(VMA/MA)│ │(Async)   │ │ SIMD     │    │
│  └────────┘ └────────┘ └────────┘ └──────────┘ └──────────┘    │
└──────────────────────────────────────────────────────────────────┘
```

### 1.2 数据流向

```
Asset (glTF/Texture) ──Import──► AssetRegistry ──Instantiate──► Entity + Components
                                                                        │
                                                          ┌─────────────┘
                                                          ▼
                                          RenderSystem::OnUpdate()
                                                          │
                                          ┌───────────────┴───────────────┐
                                          ▼                               ▼
                                   CPU Culling                     GPU Culling
                                   (Frustum/Occlusion)             (Compute Shader)
                                          │                               │
                                          └───────────────┬───────────────┘
                                                          ▼
                                                  Render Graph Build
                                                          │
                                          ┌───────────────┼───────────────┐
                                          ▼               ▼               ▼
                                    Shadow Pass     GBuffer Pass    Depth PrePass
                                          │               │               │
                                          └───────────────┼───────────────┘
                                                          ▼
                                                   Lighting Pass
                                                  (Clustered Shading)
                                                          │
                                          ┌───────────────┼───────────────┐
                                          ▼               ▼               ▼
                                        GI Pass       RT Pass       SSS Pass
                                          │               │               │
                                          └───────────────┼───────────────┘
                                                          ▼
                                                  Post-Processing
                                          ┌───────────────┼───────────────┐
                                          ▼               ▼               ▼
                                      ToneMap       AA/Upscale      HDR Output
                                                          │
                                                          ▼
                                                     SwapChain Present
```

---

## 2. 模块目录与依赖

### 2.1 模块清单

每个模块标注：**层** | **Phase** | **前置依赖** | **是否可并行**

| ID | 模块 | 层 | Phase | 依赖 | 并行组 |
|----|------|----|-------|------|--------|
| M00 | BuildSystem (CMake+vcpkg) | L0 | P1 | — | A |
| M01 | Platform (GLFW/SDL3, Thread, Mem, IO) | L0 | P1 | — | A |
| M02 | Math (GLM + SIMD + Geometry) | L0 | P1 | — | A |
| M03 | Logging (spdlog wrapper) | L0 | P1 | — | A |
| M04 | RHI Core (Device/SwapChain/Queue/CommandList) | L2 | P1 | M01,M02 | B |
| M05 | RHI Buffer/Texture/Sampler | L2 | P1 | M04 | B |
| M06 | RHI PSO + Pipeline Library | L2 | P1 | M04,M05 | B |
| M07 | RHI Bindless (Descriptor Indexing) | L2 | P2 | M06 | — |
| M08 | RHI AsyncCompute | L2 | P1 | M04 | B |
| M09 | RHI RayTracing (DXR/VulkanRT) | L2 | P4 | M06,M07 | — |
| M10 | RHI Profiling (PIX/RenderDoc markers) | L2 | P1 | M04 | B |
| M12 | SlangCompiler (Frontend + Backend) | L3 | P1 | M06,M11 | C |
| M13 | ShaderHotReload | L3 | P1 | M12 | C |
| M14 | ShaderVariantMgmt + ASD | L3 | P5 | M12 | — |
| M15 | Cpp26Reflection (^T + [:...:]) | L1 | P1 | M00 | A |
| M16 | EngineAttributes ([[engine::...]]) | L1 | P1 | M15 | A |
| M17 | Serialization (Binary + JSON + Diff) | L1 | P1 | M15,M16 | — |
| M18 | TypeRegistry (Runtime reflection DB) | L1 | P1 | M15,M16 | — |
| M19 | RenderGraph (Pass DAG + Barrier + Alias) | L4 | P1 | M06,M08 | D |
| M20 | Entity (UUID/Name/Component List) | L5 | P1 | M18 | E |
| M21 | Component (Base + Lifecycle + Query) | L5 | P1 | M18,M20 | E |
| M22 | SceneGraph (Transform Hierarchy) | L5 | P1 | M21 | E |
| M23 | PrefabSystem (Template + Override) | L5 | P2 | M22,M17 | — |
| M24 | StaticMeshComponent | L5 | P1 | M21,M22 | F |
| M25 | LightComponent (Point/Spot/Dir/Area/IES) | L5 | P1 | M21,M22 | F |
| M26 | CameraComponent | L5 | P1 | M21,M22 | F |
| M27 | SkeletalMeshComponent | L5 | P6 | M21,M22 | — |
| M28 | DecalComponent | L5 | P2 | M21,M22 | — |
| M29 | ParticleComponent | L5 | P6 | M21,M22 | — |
| M30 | VolumeComponent (PostProcess/Trigger) | L5 | P6 | M21,M22 | — |
| M31 | ReflectionProbeComponent | L5 | P2 | M21,M22 | — |
| M32 | GSComponent (3DGS/4DGS) | L5 | P7 | M21,M22 | — |
| M33 | glTFLoader (GLB + JSON + Extensions) | L4 | P1 | M17,M24 | G |
| M34 | ForwardPipeline (HDR + PBR) | L4 | P1 | M19,M24,M25,M26 | H |
| M35 | DeferredPipeline (GBuffer + LightPass) | L4 | P1 | M19,M24,M25,M26 | H |
| M36 | ForwardPlusPipeline (Tiled/Clustered) | L4 | P2 | M19,M24,M25,M26 | — |
| M37 | VisibilityBufferPipeline | L4 | P3 | M19,M36 | — |
| M38 | FullPathTracingPipeline | L4 | P8 | M19,M48 | — |
| M39 | ClusteredShading (Light Culling) | L4 | P1 | M35,M36 | I |
| M40 | ShadowMaps (CSM + PCF + PCSS) | L4 | P2 | M19,M25 | — |
| M41 | VirtualShadowMaps (VSM + SMRT) | L4 | P3 | M40,M45 | — |
| M42 | IBL (Image Based Lighting) | L4 | P2 | M35 | — |
| M43 | ReflectionProbes (Cubemap/Parallax) | L4 | P2 | M31,M35 | — |
| M44 | GPUCulling (Frustum + Hi-Z) | L4 | P2 | M19,M24 | J |
| M45 | ExecuteIndirect + DGC | L4 | P2 | M44 | J |
| M46 | GPUSceneUpload | L4 | P2 | M22,M24,M45 | J |
| M47 | VRS (Tier 1/2) | L4 | P2 | M19 | — |
| M48 | VirtualGeometry (Nanite: Cluster+LOD+SW-Rast) | L4 | P3 | M37,M45,M46 | K |
| M49 | MeshShaderPipeline (Meshlet+AS+Assemble) | L4 | P3 | M45,M46,M48 | K |
| M50 | RTXMegaGeometry (Wide BVH, 巨量几何 RT 加速) | L4 | P4 | M09,M48 | — |
| M51 | DisplacementMicromaps (DMM) | L4 | P4 | M09 | — |
| M53 | SparseVirtualTexturing (SVT) | L4 | P3 | M05,M46 | K |
| M54 | SamplerFeedback | L4 | P3 | M53 | K |
| M55 | StreamingTexturePool | L4 | P3 | M53 | — |
| M56 | RTXTextureStreaming (RTXTS) | L4 | P5 | M55 | — |
| M57 | RTXTextureFiltering (RTXTF) | L4 | P5 | M55 | — |
| M58 | LumenGI (SurfaceCache + RadianceCache) | L4 | P4 | M35,M36,M39,M09 | — |
| M59 | DDGI_RTXGI (Probe-based GI) | L4 | P4 | M09,M35 | — |
| M60 | VXGI_SDFGI (Voxel/SDF GI) | L4 | P4 | M09 | — |
| M61 | SHProbeGrid_SHaRC | L4 | P4 | M09,M59 | — |
| M62 | NeuralRadianceCache (NRC) | L4 | P5 | M09,M61 | — |
| M63 | RTPipeline (Reflections+Shadows+AO+Trans) | L4 | P4 | M09,M19 | L |
| M64 | PathTracingReference | L4 | P4 | M09,M19 | L |
| M65 | ReSTIR_DI (Direct Illumination) | L4 | P4 | M09,M63 | L |
| M66 | ReSTIR_GI (Global Illumination) | L4 | P4 | M09,M65 | L |
| M67 | ReSTIR_PT (Path Tracing) | L4 | P4 | M09,M66 | L |
| M68 | ReservoirSplatting (Forward Projection) | L4 | P5 | M09,M67 | — |
| M69 | OMM (Opacity Micromaps) | L4 | P4 | M09,M63 | L |
| M70 | SER (Shader Execution Reordering) | L4 | P4 | M09 | L |
| M71 | NRD (ReBLUR + SIGMA + ReLAX) | L4 | P4 | M63,M64,M65 | M |
| M72 | SVGF_A_SVGF (Variance Guided Filter) | L4 | P4 | M71 | M |
| M73 | StreamlineSDK (DLSS/FSR/XeSS Integration) | L4 | P5 | M19 | — |
| M74 | FrameGeneration (DLSS-FG + FSR4-FG) | L4 | P5 | M73 | — |
| M75 | RayReconstruction (DLSS 3.5/RX Transf.) | L4 | P5 | M71,M73 | — |
| M76 | NeuralMaterials + NTC | L4 | P5 | M12,M14 | — |
| M77 | DirectXLinAlg (SM6.10 linalg::Matrix) | L4 | P5 | M12 | — |
| M78 | RTXNeuralShaders (RTXNS + Slang+CV) | L4 | P5 | M12,M77 | — |
| M79 | RTXCharacterRendering (RTXCR: LSS + SSS) | L4 | P5 | M27,M78 | — |
| M80 | SpectralRendering (Wavelength-based) | L4 | P6 | M35 | — |
| M81 | NeuralAppearanceModels (NIV) | L4 | P6 | M62 | — |
| M82 | AtmosphericScattering (Rayleigh/Mie) | L4 | P6 | M35 | N |
| M83 | PhysicallyBasedSky (Bruneton/Hosek-W.) | L4 | P6 | M82 | N |
| M84 | VolumetricClouds | L4 | P6 | M82,M83 | N |
| M85 | VolumetricFog + LightShafts | L4 | P6 | M82,M25 | N |
| M86 | AerialPerspective + CloudShadow | L4 | P6 | M83,M84 | N |
| M87 | PostProcessBloom | L4 | P1 | M19 | O |
| M88 | PostProcessDOF | L4 | P6 | M19 | — |
| M89 | PostProcessMotionBlur | L4 | P1 | M19,M35 | O |
| M90 | PostProcessAutoExposure | L4 | P1 | M19 | O |
| M91 | PostProcessColorGrading | L4 | P1 | M19,M90 | O |
| M92 | PostProcessLensFlare | L4 | P6 | M19 | — |
| M93 | PostProcessCA (Chromatic Aberration) | L4 | P6 | M19 | — |
| M94 | PostProcessFilmGrain + Vignette | L4 | P6 | M19 | — |
| M95 | HDRDisplayOutput (HDR10/scRGB) | L4 | P6 | M91 | — |
| M96 | TemporalUpsampling (TAAU) | L4 | P2 | M35 | — |
| M97 | AntiAliasing_MSAA | L4 | P1 | M34 | P |
| M98 | AntiAliasing_TAA | L4 | P1 | M35 | P |
| M99 | AntiAliasing_SMAA_FXAA_CMAA | L4 | P2 | M35 | — |
| M100 | DLAA (Deep Learning AA) | L4 | P5 | M73 | — |
| M101 | ScreenSpaceAO (SSAO/GTAO/HBAO+) | L4 | P1 | M35 | Q |
| M102 | ScreenSpaceGI (SSGI/GTGI) | L4 | P4 | M35 | — |
| M103 | ScreenSpaceReflections (SSR) | L4 | P1 | M35 | Q |
| M104 | ScreenSpaceRefraction | L4 | P6 | M35 | — |
| M105 | NewtonMethodCaustics (SS Caustics) | L4 | P7 | M103 | — |
| M106 | SMS_ReSTIR_Caustics | L4 | P7 | M09,M67,M105 | — |
| M107 | VolumetricCaustics | L4 | P7 | M85,M106 | — |
| M108 | GaussianSplatting3D (Raster Pipeline) | L4 | P7 | M19,M32 | R |
| M109 | HybridMeshGSCompositing | L4 | P7 | M108 | R |
| M110 | NaniteStyleLOD_for_GS | L4 | P7 | M108,M48 | R |
| M111 | GaussianSplatting4D (Dynamic/Volumetric) | L4 | P7 | M108 | R |
| M112 | Relightable3DGS (NVOL/PLY+SH) | L4 | P7 | M108 | — |
| M113 | DeformableBetaSplatting | L4 | P7 | M108 | — |
| M114 | VR_XR_3DGS (Binocular+OpenXR) | L4 | P8 | M108 | — |
| M115 | SkeletalAnimation (GPU Skinning) | L6 | P6 | M22,M27 | S |
| M116 | BlendShapes_MorphTargets | L6 | P6 | M22,M27 | S |
| M117 | VAT (Vertex Animation Texture) | L6 | P6 | M27 | S |
| M118 | ProceduralAnimation (GPU) | L6 | P6 | M118, M01 | S |
| M119 | GPUProceduralGeneration (WorkGraph+MeshNode) | L6 | P6 | M118,M49 | — |
| M120 | EditorCore (Engine/Editor Separation) | L8 | P1 | M20,M21 | T |
| M121 | EditorViewport (Multi-Viewport) | L8 | P1 | M19,M26,M120 | T |
| M122 | EditorWorldOutliner (Entity Tree) | L8 | P1 | M20,M21,M120 | T |
| M123 | EditorDetailsPanel (Property Editor) | L8 | P1 | M18,M120,M121 | T |
| M124 | EditorUndoRedo (Command Pattern) | L8 | P1 | M120 | T |
| M125 | EditorGizmo (Translate/Rotate/Scale) | L8 | P2 | M121,M22 | — |
| M126 | EditorSnapping (Grid/Vertex/Angle) | L8 | P2 | M125 | — |
| M127 | EditorContentBrowser (Tree+Thumbnail) | L8 | P2 | M120,M17 | — |
| M128 | EditorAssetImport (Drag&Drop + Dialog) | L8 | P2 | M33,M127 | — |
| M129 | EditorAssetRegistry (Metadata + Deps) | L8 | P2 | M18,M127 | — |
| M130 | EditorMaterialEditor (Node Graph) | L8 | P3 | M12,M120 | — |
| M131 | EditorVisualScripting (Blueprint-like) | L8 | P5 | M18,M120 | — |
| M132 | EditorPIE (Play In Editor) | L8 | P4 | M120 | — |
| M133 | EditorConsole + CVar | L8 | P1 | M120 | T |
| M134 | EditorStatsProfiler (GPU Trace Viewer) | L8 | P4 | M11,M120 | — |
| M135 | EditorTerrainFoliage (Heightmap+Brush) | L8 | P6 | M120 | — |
| M136 | EditorPrefabEditor | L8 | P3 | M23,M120 | — |
| M137 | OIT (Weighted Blended OIT) | L4 | P3 | M35 | — |
| M138 | MultiView_Stereo (VR Rendering) | L4 | P6 | M19,M34 | — |
| M139 | ImpostorSystem | L4 | P3 | M48 | — |
| M140 | DrawCallMerging (GPU Instance+Indirect) | L4 | P2 | M45 | — |
| M141 | ResourceStreaming (Async Geo/Tex Load) | L4 | P2 | M46,M53 | — |
| M142 | STBN (SpatioTemporal Blue Noise) | L4 | P5 | M12 | — |
| M143 | ComputeGraphCompiler | L4 | P8 | M12,M77 | — |
| M144 | Reflex2_FrameWarp | L4 | P8 | M19 | — |
| M145 | NetworkReplication (Component RPC) | L6 | P6 | M18,M21 | — |

---

## 3. 依赖拓扑图

```
L0 Platform ──────────────────────────────────────────────────────────────
    M00 ──┬── M01 ──┬── M04 ──┬── M05 ── M06 ── M07 (P2)
    M02   │   M03   │         │          │
    M15 ──┤         │         ├── M08 ───┤
    M16 ──┤         │         │          │
          │         │         ├── M11    │
          │         │         │          │
          │         │         └── M09 ───┴── M10 (P8)
          │         │
          │         └── M12 ── M13 ── M14 (P5)
          │
L1 Reflection ───────────────────────────────────────────────────────────
    M15──M16──┬── M17
              └── M18
              │
L5 Component ────────────────────────────────────────────────────────────
    M18──M20──M21──┬── M22 ── M23 (P2)
                   │
                   ├── M24 (StaticMeshComponent)
                   ├── M25 (LightComponent)
                   ├── M26 (CameraComponent)
                   ├── M27 (SkeletalMeshComp, P6)
                   ├── M28 (DecalComp, P2)
                   ├── M29 (ParticleComp, P6)
                   ├── M30 (VolumeComp, P6)
                   ├── M31 (ReflectionProbeComp, P2)
                   └── M32 (GSComponent, P7)
                   │
L4 Rendering ────────────────────────────────────────────────────────────
    M06──M08──M19 (RenderGraph) ─────────────────────────────┐
    M17──M24──M33 (glTF Loader)                              │
    │                                                        │
    ├── M34 (Forward P1) ── M97 (MSAA)                       │
    ├── M35 (Deferred P1) ──┬── M39 (Clustered P1)          │
    │                       ├── M98 (TAA)                     │
    │                       ├── M89 (MotionBlur)              │
    │                       ├── M101 (SSAO/GTAO)              │
    │                       └── M103 (SSR)                    │
    ├── M36 (Forward+ P2)                                     │
    ├── M37 (VisBuffer P3) ── M48 (Nanite P3) ── M49 (MeshShader)
    │                                          └── M50 (MegaGeo P4)
    ├── M40 (ShadowMaps P2) ── M41 (VSM P3)                  │
    ├── M42 (IBL P2) + M43 (ReflProbes P2)                   │
    ├── M44 (GPUCull P2) ──┬── M45 (ExecIndirect P2)        │
    │                      └── M46 (GPUSceneUpload P2)       │
    ├── M47 (VRS P2)                                          │
    ├── M53 (SVT P3) ── M54/M55 ── M56/M57 (P5)              │
    │                                                        │
    ├── P4: GI+RT ──────────────────────────────────────┐    │
    │   M58 (LumenGI)                                       │
    │   M59 (DDGI) ── M61 (SHaRC) ── M62 (NRC P5)         │
    │   M60 (VXGI/SDFGI)                                   │
    │   M63 (RT Pipeline) ──┬── M64 (PathTracingRef)      │
    │                      ├── M65 (ReSTIR DI)            │
    │                      ├── M66 (ReSTIR GI)            │
    │                      ├── M67 (ReSTIR PT)            │
    │                      ├── M68 (ReservoirSplatt P5)   │
    │                      ├── M69 (OMM)                  │
    │                      └── M70 (SER)                  │
    │   M71 (NRD) ── M72 (SVGF)                           │
    │   M52 (DMM)                                         │
    │                                                      │
    ├── P5: Neural ────────────────────────────────────┐  │
    │   M73 (Streamline) ── M74 (FrameGen) ── M75 (RR) │  │
    │   M76 (NeuralMat+NTC)                             │  │
    │   M77 (LinAlg) ── M78 (RTXNS) ── M79 (RTXCR)     │  │
    │   M100 (DLAA), M142 (STBN)                       │  │
    │                                                    │
    ├── P6: VFX+Anim ──────────────────────────────────┐│
    │   M82──M83──M84──M85──M86 (Atmosphere+Vol)       ││
    │   M88 (DOF), M92-94 (PostProcess)                ││
    │   M95 (HDR Output), M104 (SS Refraction)         ││
    │   M80 (Spectral), M81 (NIV)                      ││
    │   M115──M119 (Animation)                          ││
    │   M138 (VR Stereo)                                ││
    │                                                    │
    ├── P7: Caustics+3DGS ─────────────────────────────┐│
    │   M105──M106──M107 (Caustics)                    ││
    │   M108──M109──M113 (3DGS Family)                 ││
    │                                                    │
    └── P8: Polish ────────────────────────────────────┐│
        M38 (Full PT), M114 (VR/XR 3DGS), M143 (CGC), M144 (Reflex2) ││
                                                         │
L8 Editor ───────────────────────────────────────────────┘
    M120──┬── M121 (Viewport)
          ├── M122 (Outliner)
          ├── M123 (Details Panel)
          ├── M124 (Undo/Redo)
          └── M133 (Console)
          │
    P2+: M125 (Gizmo), M126 (Snapping)
         M127 (ContentBrowser), M128 (AssetImport)
         M129 (AssetRegistry)
    P3:  M130 (MaterialEditor), M136 (PrefabEditor)
    P4:  M132 (PIE), M134 (Profiler)
    P5:  M131 (VisualScripting)
    P6:  M135 (TerrainFoliage)
```

---

## 4. 任务划分（WBS）

### 4.1 任务粒度说明

| 粒度 | 代码 | 工期 | 说明 |
|------|------|------|------|
| Epic | **EP-XX** | 多周 | 一个完整模块或子系统 |
| Story | **ST-XX.YY** | 1-2 周 | 模块内一个可独立交付的特性 |
| Task | — | 1-3 天 | Story 的具体实现步骤 |

### 4.2 WBS 一览

```
HugEngine (总计 ~119 周)
│
├── Phase 1 · 核心骨架 (21 周) ─────────────────────────────────
│   ├── EP-01 项目基础设施 (2w)
│   │   ├── ST-01.01 CMake + vcpkg 构建系统
│   │   ├── ST-01.02 目录结构 + 编码规范 + CI
│   │   └── ST-01.03 spdlog 日志集成
│   │
│   ├── EP-02 平台抽象层 (2w)  [可并行]
│   │   ├── ST-02.01 窗口系统 (GLFW/SDL3) + 输入
│   │   ├── ST-02.02 线程池 / Job System (Taskflow)
│   │   ├── ST-02.03 内存分配器 (VMA/D3D12MA 封装)
│   │   └── ST-02.04 异步文件 I/O
│   │
│   ├── EP-03 数学库 (2w)  [可并行]
│   │   ├── ST-03.01 GLM 封装 (vec/mat/quat 统一接口)
│   │   ├── ST-03.02 SIMD 加速 (SSE/AVX/NEON)
│   │   └── ST-03.03 几何工具 (AABB/Frustum/Ray/Sphere)
│   │
│   ├── EP-04 C++26 反射系统 (2w)  [可并行]
│   │   ├── ST-04.01 ^T 反射操作符 + meta::info 封装
│   │   ├── ST-04.02 [[engine::]] 属性系统 (category/range/tooltip...)
│   │   ├── ST-04.03 编译期序列化 (Binary + JSON)
│   │   └── ST-04.04 运行时类型注册表 (TypeRegistry)
│   │
│   ├── EP-05 RHI 抽象层 · 核心 (4w)  ★ 关键路径
│   │   ├── ST-05.01 RHI 接口定义 (IRHI Device/SwapChain/Queue/CommandList)
│   │   ├── ST-05.02 Vulkan 1.3+ 后端实现
│   │   ├── ST-05.03 D3D12 SM 6.6+ 后端实现
│   │   ├── ST-05.04 RHI Buffer/Texture/Sampler 抽象
│   │   ├── ST-05.05 RHI PSO + Pipeline Library 缓存
│   │   ├── ST-05.06 Resource Barrier 自动管理
│   │   └── ST-05.07 GPU Profiling Markers (PIX/RenderDoc/Nsight)
│   │
│   ├── EP-06 Slang 着色器编译管线 (2w)
│   │   ├── ST-06.01 Slang → SPIR-V 编译路径
│   │   ├── ST-06.02 Slang → DXIL 编译路径
│   │   └── ST-06.03 Shader Hot Reload 系统
│   │
│   ├── EP-07 Render Graph (3w)  ★ 关键路径
│   │   ├── ST-07.01 Pass 依赖图数据结构 (DAG)
│   │   ├── ST-07.02 资源生命周期追踪 + 自动 Barrier 推导
│   │   ├── ST-07.03 资源别名 (Transient Resource / Memory Reuse)
│   │   ├── ST-07.04 Async Compute Pass 调度
│   │   └── ST-07.05 帧资源环形分配器 (Ring Buffer Allocator)
│   │
│   ├── EP-08 Actor-Component 架构 (2w)  ★ 架构基石
│   │   ├── ST-08.01 Entity (UUID/Name/ComponentList) 实现
│   │   ├── ST-08.02 Component 基类 + 生命周期 (Create/Start/Update/Destroy)
│   │   ├── ST-08.03 Component 查询 (World::Query<...>)
│   │   ├── ST-08.04 Scene Graph + Transform 层级 (Dirty Flag 传播)
│   │   ├── ST-08.05 基础渲染组件 (StaticMesh/Light/Camera)
│   │   └── ST-08.06 Entity/Component 序列化 (基于 C++26 反射)
│   │
│   ├── EP-09 基础前向渲染 (2w)
│   │   ├── ST-09.01 HDR 渲染目标 + ACES Tone Mapping
│   │   ├── ST-09.02 PBR Metallic-Roughness 着色模型
│   │   └── ST-09.03 MSAA 集成
│   │
│   ├── EP-10 基础延迟渲染 (1w)
│   │   ├── ST-10.01 GBuffer (Albedo/Normal/Roughness/Metallic/Depth)
│   │   ├── ST-10.02 Clustered Shading 光照剔除
│   │   └── ST-10.03 TAA 集成
│   │
│   ├── EP-11 glTF 2.0 加载器 (1w)
│   │   ├── ST-11.01 GLB + JSON 解析
│   │   ├── ST-11.02 PBR 材质 → MaterialAsset 转换
│   │   └── ST-11.03 Mesh → Entity+StaticMeshComponent 自动装配
│   │
│   └── EP-12 基础编辑器框架 (2w)  [部分可并行 EP-09~11]
│       ├── ST-12.01 Editor/Engine 进程分离架构
│       ├── ST-12.02 Viewport 渲染 (Dear ImGui 嵌入)
│       ├── ST-12.03 World Outliner (Entity 层级树)
│       ├── ST-12.04 Details Panel (反射驱动属性编辑)
│       ├── ST-12.05 Undo/Redo 命令栈
│       └── ST-12.06 Console + CVar 系统
│
├── Phase 2 · GPU Driven + 组件扩展 (14 周) ──────────────────
│   ├── EP-13 GPU 视锥剔除 (2w)
│   │   ├── ST-13.01 Compute Shader 视锥剔除管线
│   │   └── ST-13.02 剔除结果写入 Indirect Draw Buffer
│   │
│   ├── EP-14 Hi-Z 遮挡剔除 (2w)
│   │   ├── ST-14.01 深度金字塔构建 (Multi-level Downsample)
│   │   └── ST-14.02 两阶段遮挡查询 (复用上一帧 + 当前帧验证)
│   │
│   ├── EP-15 ExecuteIndirect + DGC (2w)
│   │   ├── ST-15.01 Multi-Draw Indirect (D3D12)
│   │   └── ST-15.02 VK_EXT_device_generated_commands 集成
│   │
│   ├── EP-16 GPU Scene Upload (2w)
│   │   ├── ST-16.01 Persistent Ring Buffer + GPU Transform 更新
│   │   └── ST-16.02 Component Dirty Flag → GPU Upload 管线
│   │
│   ├── EP-17 Bindless Resources (2w)
│   │   ├── ST-17.01 Descriptor Indexing (Texture2D[], StructuredBuffer[])
│   │   └── ST-17.02 Material/Texture ID → Bindless Index 映射
│   │
│   ├── EP-18 Shadow Maps + IBL (2w)
│   │   ├── ST-18.01 CSM (Cascaded Shadow Maps) + PCF 软阴影
│   │   ├── ST-18.02 IBL (Split-Sum Approximation) 镜面反射
│   │   └── ST-18.03 Reflection Probe Component + Cubemap 捕获
│   │
│   ├── EP-19 VRS + 编辑器增强 (1w)
│   │   ├── ST-19.01 VRS Tier 1 (Per-Draw) + Tier 2 (Screen-Space Image)
│   │   └── ST-19.02 编辑器 Gizmo + Snapping + Content Browser
│   │
│   └── EP-20 组件扩展 + Prefab (1w)
│       ├── ST-20.01 DecalComponent + ReflectionProbeComponent
│       ├── ST-20.02 Prefab 系统 (模板创建 / 实例化 / Override 存储)
│       └── ST-20.03 Asset Registry (异步扫描 + 依赖图 + 缩略图)
│
├── Phase 3 · 高级几何 (14 周) ────────────────────────────────
│   ├── EP-21 Mesh Shader + Meshlet Pipeline (3w)  ★ 关键路径
│   │   ├── ST-21.01 Meshlet 离线预计算工具 (meshoptimizer 集成)
│   │   ├── ST-21.02 Amplification Shader 剔除 (Frustum + Cone + HiZ)
│   │   └── ST-21.03 Mesh Shader GPU 图元装配
│   │
│   ├── EP-22 Virtualized Geometry · Nanite (4w)  ★ 最大单模块
│   │   ├── ST-22.01 Cluster 划分 + LOD 层次构建
│   │   ├── ST-22.02 运行时 LOD 选择 (Screen-Space Error + Budget)
│   │   ├── ST-22.03 软件光栅化 (Small Triangle) + HW 光栅化 (Large)
│   │   └── ST-22.04 Visibility Buffer 渲染 (Material 延迟查询)
│   │
│   ├── EP-23 Virtual Shadow Maps (2w)
│   │   ├── ST-23.01 VSM 页表管理 (Virtual Page Table + LRU Cache)
│   │   └── ST-23.02 SMRT 软阴影 + Contact Shadows
│   │
│   ├── EP-24 Virtual Texturing (3w)
│   │   ├── ST-24.01 SVT (Tiled Resources / Vulkan Sparse)
│   │   ├── ST-24.02 Sampler Feedback + 页表更新
│   │   └── ST-24.03 Streaming Texture Pool + Feedback-based Eviction
│   │
│   └── EP-25 编辑器增强 (2w)
│       ├── ST-25.01 Material Editor (Node Graph + Slang 代码生成)
│       ├── ST-25.02 Prefab Editor + Asset Thumbnail
│       └── ST-25.03 OIT (Weighted Blended) + Impostor 系统
│
├── Phase 4 · GI + Ray Tracing (18 周) ───────────────────────
│   ├── EP-26 Lumen 式 GI (4w)
│   │   ├── ST-26.01 Surface Cache (Card-based Mesh Parameterization)
│   │   ├── ST-26.02 Radiance Cache (Screen-Space Probes + SH)
│   │   └── ST-26.03 Final Gather (SS GI + HW RT 回退)
│   │
│   ├── EP-27 DDGI / RTXGI (2w)
│   │   ├── ST-27.01 DDGI 探针放置 + 光照注入
│   │   └── ST-27.02 SH Probe Grid + SHaRC 空间哈希缓存
│   │
│   ├── EP-28 RT 管线基础 (3w)
│   │   ├── ST-28.01 TLAS/BLAS 构建 + 动态更新
│   │   ├── ST-28.02 RT Reflections + RT Shadows + RTAO
│   │   ├── ST-28.03 RT Translucency + OMM 集成
│   │   └── ST-28.04 Shader Execution Reordering (SER)
│   │
│   ├── EP-29 ReSTIR 家族 (3w)
│   │   ├── ST-29.01 ReSTIR DI (多光源直接光照时空重采样)
│   │   ├── ST-29.02 ReSTIR GI (间接光照时空重采样)
│   │   └── ST-29.03 ReSTIR PT (路径追踪 GRIS 重采样)
│   │
│   ├── EP-30 NRD 降噪器 (2w)
│   │   ├── ST-30.01 ReBLUR (漫反射/镜面降噪)
│   │   ├── ST-30.02 SIGMA (阴影降噪)
│   │   └── ST-30.03 ReLAX (ReSTIR 信号降噪) + SVGF 自研
│   │
│   ├── EP-31 Path Tracing Reference (1w)
│   │   └── ST-31.01 完整路径追踪参考模式 (离线品质对标)
│   │
│   ├── EP-32 RTX Mega Geometry + DMM (1w)
│   │   ├── ST-32.01 分区 TLAS + Wide BVH (植被密集场景)
│   │   └── ST-32.02 Displacement Micromaps 集成
│   │
│   └── EP-33 编辑器 PIE + Profiler (2w)
│       ├── ST-33.01 Play In Editor (模拟/独立进程)
│       └── ST-33.02 Stats/Profiler + GPU Capture 一键截帧
│
├── Phase 5 · 神经网络渲染 (14 周) ──────────────────────────
│   ├── EP-34 Streamline SDK 集成 (2w)
│   │   ├── ST-34.01 DLSS Super Resolution (Transformer 4.5+)
│   │   ├── ST-34.02 FSR 4.1 + XeSS
│   │   └── ST-34.03 DLAA 集成
│   │
│   ├── EP-35 Frame Generation + Ray Reconstruction (2w)
│   │   ├── ST-35.01 DLSS Multi Frame Generation (6×)
│   │   └── ST-35.02 Ray Reconstruction (Transformer 降噪)
│   │
│   ├── EP-36 Neural Radiance Cache (2w)
│   │   └── ST-36.01 NRC 在线训练 + 推理管线 + SHaRC 集成
│   │
│   ├── EP-37 Neural Materials + NTC (2w)
│   │   ├── ST-37.01 Neural Texture Compression (8× VRAM 节省)
│   │   └── ST-37.02 Neural Materials (离线材质 → 神经网络压缩)
│   │
│   ├── EP-38 DirectX LinAlg + RTXNS (2w)
│   │   ├── ST-38.01 SM 6.10 linalg::Matrix + Wave Matrix
│   │   ├── ST-38.02 RTX Neural Shaders (Slang + Cooperative Vectors)
│   │   └── ST-38.03 Variable Group Shared Memory 利用
│   │
│   ├── EP-39 RTX Character Rendering (2w)
│   │   ├── ST-39.01 Linear Swept Spheres (LSS) 毛发渲染
│   │   └── ST-39.02 Subsurface Scattering (SSS) 皮肤
│   │
│   └── EP-40 高级着色器 + 可视化脚本 (2w)
│       ├── ST-40.01 Advanced Shader Delivery (ASD) 消除卡顿
│       ├── ST-40.02 STBN (时空蓝噪声) 纹理生成
│       └── ST-40.03 Node-based Visual Scripting 初版
│
├── Phase 6 · 大气 + 后处理 + 动画 (14 周) ──────────────────
│   ├── EP-41 大气散射 + 天空 (2w)
│   │   ├── ST-41.01 Rayleigh/Mie 散射 + Bruneton 天空模型
│   │   └── ST-41.02 Aerial Perspective + 物理天空
│   │
│   ├── EP-42 体积渲染 (2w)
│   │   ├── ST-42.01 Volumetric Fog + Light Shafts (God Rays)
│   │   └── ST-42.02 Volumetric Clouds + Cloud Shadows
│   │
│   ├── EP-43 完整后处理栈 (2w)
│   │   ├── ST-43.01 DOF (Circle of Confusion) + Motion Blur
│   │   ├── ST-43.02 Lens Flare + Chromatic Aberration + Film Grain + Vignette
│   │   └── ST-43.03 HDR10 / scRGB Display Output
│   │
│   ├── EP-44 动画系统 (4w)
│   │   ├── ST-44.01 骨骼动画 (Skeleton + AnimationClip + GPU Skinning)
│   │   ├── ST-44.02 Blend Shapes / Morph Targets
│   │   ├── ST-44.03 Vertex Animation Texture (VAT)
│   │   └── ST-44.04 GPU 程序化动画 (Compute Shader 驱动)
│   │
│   ├── EP-45 地形 + 植被 (2w)
│   │   ├── ST-45.01 Heightmap Terrain + 多层材质混合
│   │   └── ST-45.02 GPU 植被实例化 + Foliage 笔刷
│   │
│   └── EP-46 高级渲染 (2w)
│       ├── ST-46.01 Spectral Rendering (波长级渲染)
│       ├── ST-46.02 Neural Appearance Models (NIV)
│       └── ST-46.03 GPU Procedural Generation (Work Graphs + Mesh Nodes)
│
├── Phase 7 · 高斯泼溅 + 焦散 (12 周) ──────────────────────
│   ├── EP-47 3DGS 光栅化管线 (3w)  ★ 新图元
│   │   ├── ST-47.01 GPU Gaussian 排序 (Radix Sort) + 光栅化
│   │   ├── ST-47.02 GSComponent 实现 (挂载到 Entity)
│   │   └── ST-47.03 glTF KHR_gaussian_splatting 导入
│   │
│   ├── EP-48 Hybrid Mesh + GS 合成 (2w)
│   │   ├── ST-48.01 深度感知混合 (GS → Depth Buffer → Compose)
│   │   └── ST-48.02 Nanite-style LOD for GS (Screen-Space Error + Splat Compaction)
│   │
│   ├── EP-49 4DGS + Beta Splatting (2w)
│   │   ├── ST-49.01 4DGS 时间维度扩展 (Volumetric Video 播放)
│   │   └── ST-49.02 Deformable Beta Splatting (参数 -55%, 速度 1.5×)
│   │
│   ├── EP-50 实时焦散 · 屏幕空间 (2w)
│   │   ├── ST-50.01 Newton's Method 屏幕空间折射 + 焦散 (JCGT 2026)
│   │   └── ST-50.02 水面/玻璃焦散效果
│   │
│   └── EP-51 实时焦散 · 无偏 (3w)
│       ├── ST-51.01 SMS (Specular Manifold Sampling) + ReSTIR 时空复用
│       ├── ST-51.02 体积焦散 (Markov Chain Path Guiding)
│       └── ST-51.03 水下焦散场景
│
└── Phase 8 · 打磨与发布 (10 周) ────────────────────────────
    ├── EP-52 WebGPU 后端 (2w)
    │   └── ST-52.01 RHI WebGPU Backend (浏览器部署)
    │
    ├── EP-53 性能优化 (2w)
    │   ├── ST-53.01 PSO 预编译 Pipeline (全量预热)
    │   ├── ST-53.02 Memory Defragmentation + RTXMU 集成
    │   └── ST-53.03 Reflex 2 Frame Warp 集成
    │
    ├── EP-54 全路径追踪管线 (1w)
    │   └── ST-54.01 Full Path Tracing + ReSTIR PT → 替换混合管线
    │
    ├── EP-55 VR/XR 3DGS (1w)
    │   └── ST-55.01 Binocular Stereo Rendering + OpenXR 集成
    │
    ├── EP-56 文档 + 示例 (2w)
    │   ├── ST-56.01 API 文档 (Doxygen + 手动)
    │   ├── ST-56.02 Shader 编写指南
    │   └── ST-56.03 示例项目 (室内/室外/材质展示)
    │
    └── EP-57 Compute Graph Compiler + 发布 (1w)
        ├── ST-57.01 DirectX Compute Graph Compiler 集成 (ML 图编译器)
        └── ST-57.02 v0.1.0 发布 + Changelog
```

---

## 5. 分阶段实施计划

### 5.1 阶段总览

```
Phase 1  █████████████████████ (21w)  核心骨架：引擎能跑 + 编辑器能看
Phase 2  ██████████████       (14w)  GPU Driven：消除 CPU 瓶颈 + 组件生态
Phase 3  ██████████████       (14w)  高级几何：Nanite + VSM + VT
Phase 4  ██████████████████   (18w)  GI+RT：完整实时光照系统
Phase 5  ██████████████       (14w)  神经渲染：AI 驱动管线
Phase 6  ██████████████       (14w)  大气/后处理/动画：视觉完整
Phase 7  ████████████         (12w)  高斯泼溅+焦散：新图元
Phase 8  ██████████           (10w)  打磨：WebGPU + 优化 + 发布
        ─────────────────────────────────────────────
        总计 115 周 ≈ 2.2 年（按单人或小团队线性估算）
```

### 5.2 阶段依赖关系

```
P1 ──► P2 ──► P3 ──► P4 ──► P5 ──► P6 ──► P7 ──► P8
 │              │      │      │      │      │
 │              │      │      │      │      └── 3DGS + Caustics 独立
 │              │      │      │      └── 动画系统可部分提前
 │              │      │      └── NRD 可部分提前到 P3
 │              │      └── SVT 可部分提前到 P2
 │              └── Prefab, AssetRegistry 可提前
 └── 并行开发: 数学库 + 反射 + 平台 (前 6 周)
```

### 5.3 每阶段可并行任务标注

```
Phase 1 (最大并行度: 4 条线)
  [线A] M01, M02, M03 (Platform + Math + Log)  ── 2w
  [线B] M15, M16, M18 (C++26 Reflection)       ── 2w  } 前 6 周
  [线C] M04→M05→M06 (RHI Core)                 ── 4w  } 可并行
  [线D] M12→M13 (Slang Compiler)               ── 2w  }
  汇合点 ──► M19 (RenderGraph) 3w ──► M20,M21,M22 (Component) 2w
           ──► M34,M35 (Forward+Deferred) 3w
           ──► M33 (glTF) 1w
           ──► M120-M133 (Editor) 2w

Phase 2 (最大并行度: 3 条线)
  [线A] M44→M45→M46 (GPU Culling + Upload)
  [线B] M40, M42, M43 (Shadows + IBL)
  [线C] M07, M17, M47 (Bindless + VRS)

Phase 3 (最大并行度: 2 条线)
  [线A] M48→M49 (Nanite + MeshShader)
  [线B] M53→M54→M55 (Virtual Texturing)
  汇合 ──► M41 (VSM)

Phase 4 (最大并行度: 3 条线)
  [线A] M58 (Lumen GI)
  [线B] M59, M61 (DDGI + SHaRC)
  [线C] M63→M65→M66→M67 (RT Pipeline + ReSTIR)
  汇合 ──► M71 (NRD Denoising)
```

### 5.4 关键路径分析

```
关键路径 (决定总工期):
  M04(RHI) → M06(PSO) → M19(RenderGraph) → M34/M35(Pipelines)
  → M44(GPUCull) → M48(Nanite) → M58(Lumen) → M63(RT)
  → M73(Streamline) → M108(3DGS) → M38(FullPT)
  
  关键路径总长: ~82 周 (剩余 33 周可并行消化)

主要缓冲:
  - Phase 1 反射系统可独立于 RHI 开发
  - 编辑器可在 Phase 1-3 与渲染并行迭代
  - Animation 可较早启动 (不依赖 RT/GI)
  - 3DGS 和 Caustics 相对独立
```

---

## 6. 核心接口设计

### 6.1 RHI 抽象层

```cpp
// ============================================
// RHI 核心接口 (L2) — 所有渲染的底层契约
// ============================================

// RHI 设备能力查询
struct RHIDeviceCaps {
    uint32_t maxBindlessDescriptors;
    bool supportsRayTracing;          // DXR 1.0 / Vulkan RT
    bool supportsMeshShaders;
    bool supportsWorkGraphs;
    bool supportsCooperativeVectors;  // SM 6.9+
    bool supportsLinearAlgebra;       // SM 6.10 linalg::Matrix
    bool supportsSER;                 // Shader Execution Reordering
    bool supportsOMM;                 // Opacity Micromaps
    bool supportsVRS;
    bool supportsSamplerFeedback;
};

// 设备抽象 (创建一次，持有整个 GPU 上下文)
class IRHIDevice {
public:
    virtual RHIDeviceCaps GetCaps() const = 0;
    
    // 资源创建
    virtual Ref<IRHIBuffer>       CreateBuffer(const BufferDesc&) = 0;
    virtual Ref<IRHITexture>      CreateTexture(const TextureDesc&) = 0;
    virtual Ref<IRHISampler>      CreateSampler(const SamplerDesc&) = 0;
    virtual Ref<IRHIShader>       CreateShader(ShaderStage, Span<const uint8_t> bytecode) = 0;
    virtual Ref<IRHIPipelineState> CreatePSO(const PipelineStateDesc&) = 0;
    
    // 加速结构
    virtual Ref<IRHIAS>           CreateBLAS(const BLASDesc&) = 0;
    virtual Ref<IRHIAS>           CreateTLAS(const TLASDesc&) = 0;
    
    // 命令
    virtual Ref<IRHICommandList>  CreateCommandList(QueueType) = 0;
    virtual void                  ExecuteCommandLists(Span<Ref<IRHICommandList>>) = 0;
    
    // 资源转换 (D3D12 Enhanced Barriers / Vulkan Layout Transition)
    virtual void                  ResourceBarrier(IRHICommandList*, Span<BarrierDesc>) = 0;
};

// SwapChain
class IRHISwapChain {
public:
    virtual Ref<IRHITexture> GetCurrentBackBuffer() = 0;
    virtual void             Present(bool vsync) = 0;
    virtual void             Resize(uint32_t w, uint32_t h) = 0;
};
```

### 6.2 Component 系统

```cpp
// ============================================
// Component 架构 (L5) — 场景组织的核心抽象
// ============================================

// Entity: 轻量 ID 容器，零虚函数
class Entity {
public:
    using ID = uint64_t;  // UUID
    
    ID                  GetID() const;
    std::string_view    GetName() const;
    void                SetName(std::string name);
    
    // Component 操作
    template<typename T, typename... Args>
        requires std::derived_from<T, Component>
    T*                  AddComponent(Args&&...);
    
    template<typename T>
        requires std::derived_from<T, Component>
    T*                  GetComponent();
    
    template<typename T>
        requires std::derived_from<T, Component>
    void                RemoveComponent();
    
    // 遍历所有 Component (用于序列化)
    void                ForEachComponent(std::function<void(Component&)> callback);
    
    World*              GetWorld() const;
    bool                IsActive() const;
    void                SetActive(bool active);

private:
    ID                          m_ID;
    std::string                 m_Name;
    World*                      m_World;
    bool                        m_Active = true;
    // 内部: 按 type_hash 索引的 Component 存储
    std::unordered_map<uint64_t, std::unique_ptr<Component>> m_Components;
};

// Component 基类 — 基于 C++26 反射
// 任何 [[engine::component]] 标记的类型自动继承 Component 接口
class Component {
public:
    virtual ~Component() = default;
    
    // 生命周期 (由 World 调用，不要手动调用)
    virtual void OnCreate()  {}
    virtual void OnStart()   {}
    virtual void OnUpdate(float deltaTime) {}
    virtual void OnDestroy() {}
    virtual void OnEnable()  {}
    virtual void OnDisable() {}
    
    Entity*             GetEntity() const { return m_Entity; }
    bool                IsActive()  const { return m_Active && m_Entity && m_Entity->IsActive(); }
    
    // 编译期依赖声明 (C++26 属性驱动)
    // [[engine::require<Transform>]]  → 自动在 AddComponent 时检查

protected:
    Entity* m_Entity = nullptr;
    bool    m_Active = true;
    bool    m_Started = false;
    
    friend class World;
};

// TransformComponent — 每个 Entity 必须有一个
struct [[engine::component]]
       [[engine::display_name("Transform")]]
       TransformComponent : Component {
    
    [[engine::category("Transform")]]
    float3 position = float3(0.0f);
    
    [[engine::category("Transform")]]
    quat rotation = quat::identity();
    
    [[engine::category("Transform")]]
    float3 scale = float3(1.0f);
    
    // 层级
    Entity* parent = nullptr;
    std::vector<Entity*> children;
    
    // 延迟求值的世界矩阵
    float4x4 GetLocalMatrix() const;
    float4x4 GetWorldMatrix() const;  // 仅在 Dirty 时重算
    
    // Dirty Flag 自动传播
    void MarkDirty();
};

// 渲染 Component 示例
struct [[engine::component]]
       [[engine::display_name("Static Mesh")]]
       [[engine::require<TransformComponent>]]   // 编译期依赖
       StaticMeshComponent : Component {
    
    [[engine::category("Mesh")]]
    [[engine::asset_picker(".gltf", ".glb")]]
    AssetRef<StaticMeshAsset> mesh;
    
    [[engine::category("Materials")]]
    std::vector<AssetRef<MaterialAsset>> material_overrides;
    
    [[engine::category("Rendering")]]
    bool cast_shadow = true;
    
    [[engine::category("Rendering")]]
    bool visible_in_reflections = true;
};

// World::Query — 高效的 Component 遍历
template<typename... Components>
auto World::Query()
{
    // 内部: 利用 TypeRegistry 建立 Component type_hash → Entity 集合的索引
    // 对多 Component 查询，从小集合开始交集过滤
    return QueryResult<Components...>(m_ComponentIndex);
}
```

### 6.3 Render Graph

```cpp
// ============================================
// Render Graph (L4) — 帧渲染的总调度器
// ============================================

class RenderGraph {
public:
    // Pass 声明 (在 Record 阶段)
    template<typename SetupFunc, typename ExecuteFunc>
    RGPassID AddPass(const char* name, SetupFunc&& setup, ExecuteFunc&& execute);
    
    // 资源声明 — 自动追踪生命周期和 Barrier
    template<typename T>
    RGResourceHandle<T> CreateTexture(const char* name, const TextureDesc& desc);
    
    template<typename T>
    RGResourceHandle<T> CreateBuffer(const char* name, const BufferDesc& desc);
    
    // 读写声明 — 自动推导 Barrier
    template<typename T>
    void Read(RGPassID pass, RGResourceHandle<T> resource);
    
    template<typename T>
    void Write(RGPassID pass, RGResourceHandle<T> resource);
    
    // 编译 + 执行
    void Compile();    // 解析 DAG → 确定资源生命周期 → 插入 Barrier → 内存别名
    void Execute();    // 提交到 GPU (可重叠 Async Compute)
    
    // 帧间持久化资源
    template<typename T>
    RGResourceHandle<T> ImportExternalTexture(const char* name, IRHITexture* tex);
    
    // GPU 时间统计
    struct PassStats {
        const char* name;
        float       gpu_time_ms;
        uint32_t    barrier_count;
        uint32_t    transient_memory_bytes;
    };
    Span<const PassStats> GetStats() const;
};

// 典型使用 (Deferred Rendering Frame)
void RenderSystem::RenderFrame(World& world, CameraComponent& camera) {
    RenderGraph rg;
    
    // 1. 深度预 Pass
    auto depth_pass = rg.AddPass("DepthPrePass",
        [&](auto& builder) {
            auto depth = builder.CreateTexture("Depth", {w, h, FORMAT_D32_FLOAT});
            builder.Write(depth);
        },
        [&](auto& ctx) {
            ctx.DrawMeshes(world, camera, MATERIAL_MODE_DEPTH_ONLY);
        }
    );
    
    // 2. GBuffer Pass
    auto gbuffer_pass = rg.AddPass("GBuffer",
        [&](auto& builder) {
            auto albedo  = builder.CreateTexture("GBuffer_Albedo", {w, h, FORMAT_RGBA8_UNORM});
            auto normal  = builder.CreateTexture("GBuffer_Normal", {w, h, FORMAT_RGBA16_FLOAT});
            auto rough_metal = builder.CreateTexture("GBuffer_RoughMetal", {w, h, FORMAT_RG8_UNORM});
            builder.Read(depth_pass.output);
            builder.Write(albedo);
            builder.Write(normal);
            builder.Write(rough_metal);
        },
        [&](auto& ctx) { ctx.DrawGBuffer(world, camera); }
    );
    
    // 3. 光照 Pass (读 GBuffer)
    auto lighting_pass = rg.AddPass("DeferredLighting",
        [&](auto& builder) {
            auto lit = builder.CreateTexture("LightingResult", {w, h, FORMAT_RGBA16_FLOAT});
            builder.Read(gbuffer_pass.albedo);
            builder.Read(gbuffer_pass.normal);
            builder.Read(gbuffer_pass.rough_metal);
            builder.Read(depth_pass.depth);
            builder.Write(lit);
        },
        [&](auto& ctx) {
            ctx.ClusteredLighting(world, camera);  // Clustered Shading
        }
    );
    
    // 4. 后处理
    // ... (SSR, Bloom, DOF, ToneMap, TAA 等)
    
    rg.Compile();
    rg.Execute();
}
```

### 6.4 虚拟几何系统

```cpp
// ============================================
// Nanite 式虚拟几何 (L4)
// ============================================

// Cluster: 基本渲染单元 (max 128 triangles)
struct MeshCluster {
    uint32_t            cluster_id;
    AABB                bounds;
    float               parent_lod_error;
    float               self_lod_error;
    BLASHandle          blas_handle;        // GPU 加速结构
};

// Cluster Group: LOD 层次中的一个节点
struct ClusterGroup {
    std::vector<MeshCluster> clusters;
    float                    max_parent_error;
    uint32_t                 lod_level;
};

// 虚拟几何资源 (离线预计算)
class VirtualGeometryAsset {
public:
    // 加载时预计算
    void BuildFromMesh(const StaticMeshData& mesh);
    
    // 查询
    const ClusterGroup& GetLODGroup(uint32_t lod) const;
    uint32_t            GetLODCount() const;
    
    // 加速结构构建
    void                BuildBLAS(IRHIDevice* device);
    void                RefitBLAS(IRHIDevice* device,
                               Span<const float3> animated_vertices);
    
private:
    std::vector<ClusterGroup> m_LODGroups;
    BLASHandle          m_StaticBLAS;
};

// GPU 端 LOD 选择 (Compute Shader)
// 每个 Cluster 一条线程: 
//   screen_error = project(cluster.bounds) → select LOD → append to indirect draw
```

### 6.5 Slang 着色器架构

```hlsl
// ============================================
// Slang Shader 示例 (HugEngine 着色器标准)
// ============================================

// --- 共享头文件: hugengine/ShaderParams.slang ---

// 引擎自动注入的 Uniform Buffer (每帧)
[[vk::binding(0, 0)]] [[d3d12::binding(0, 0)]]
cbuffer FrameUniforms {
    float4x4    ViewMatrix;
    float4x4    ProjMatrix;
    float4x4    ViewProjMatrix;
    float4x4    InvViewProjMatrix;
    float3      CameraWorldPos;
    float       Time;
    float2      RenderTargetSize;
    float2      RenderTargetInvSize;
    uint        FrameIndex;
};

// Bindless 资源 (Descriptor Indexing)
[[vk::binding(1, 0)]] [[d3d12::binding(1, 0)]]
StructuredBuffer<MeshData> SceneMeshes[];       // 无界数组

[[vk::binding(2, 0)]] [[d3d12::binding(2, 0)]]
Texture2D SceneTextures[];

// --- 材质函数签名 ---
// Slang 接口: 每种材质实现此接口
interface IMaterial {
    float3 EvaluateAlbedo(float2 uv);
    float  EvaluateRoughness(float2 uv);
    float  EvaluateMetallic(float2 uv);
    float3 EvaluateNormal(float2 uv, float3 geometryNormal, float3 tangent);
};

// --- GBuffer 顶点/像素着色器 ---
struct GBufferVSInput {
    uint meshID      : MESH_ID;        // Bindless index
    uint instanceID  : SV_InstanceID;
};

struct GBufferVSOutput {
    float4 position  : SV_Position;
    float3 worldPos  : WORLD_POS;
    float3 normal    : NORMAL;
    float3 tangent   : TANGENT;
    float2 uv        : TEXCOORD0;
    uint   materialID: MATERIAL_ID;     // 材质索引 (延迟查询)
};

// --- Clustered Shading Compute Shader ---
[shader("compute")]
[numthreads(64, 1, 1)]
void ClusteredLightCulling(
    StructuredBuffer<LightData> Lights,          // 场景所有光源
    StructuredBuffer<ClusterAABB> Clusters,      // 视锥空间 Cluster AABB
    RWStructuredBuffer<uint> LightIndexList,     // 输出: 每个 Cluster 可见光源列表
    RWStructuredBuffer<uint> LightGrid,          // 输出: List offset + count
    uint3 groupID       : SV_GroupID,
    uint  groupThreadID : SV_GroupIndex)
{
    uint clusterIndex = groupID.x;
    ClusterAABB cluster = Clusters[clusterIndex];
    
    // ... Z-Test + AABB-Light 相交测试 ...
}

// --- ClosestHit Shader 示例 ---
[shader("closesthit")]
void ClosestHitShader(inout Payload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    // 从 Instance ID 和 Primitive Index 查找材质
    uint materialID = InstanceMaterialIDs[InstanceID()];
    MaterialData mat = MaterialBuffer[materialID];
    payload.albedo = mat.baseColor;
    payload.emissive = mat.emissive;
    
    float3 barycentrics = float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y,
                                  attribs.barycentrics.x, attribs.barycentrics.y);
    float3 hitWorldPos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    payload.worldPos = hitWorldPos;
}
```

### 6.6 编辑器与引擎分离

```cpp
// ============================================
// Editor / Engine 分离架构 (L8)
// ============================================

// Engine 核心 — 零 UI 依赖，可独立运行
class Engine {
public:
    void Initialize(const EngineConfig& config);
    void LoadWorld(const std::string& path);
    void Tick(float deltaTime);         // 运行一帧
    void Shutdown();
    
    World*                GetActiveWorld();
    RenderGraph&          GetRenderGraph();
    IRHIDevice*           GetRHIDevice();
    const FrameStats&     GetFrameStats() const;
    
private:
    std::unique_ptr<IRHIDevice>   m_RHIDevice;
    std::unique_ptr<RenderGraph>  m_RenderGraph;
    std::unique_ptr<World>        m_ActiveWorld;
    std::unique_ptr<JobSystem>    m_JobSystem;
};

// Editor — 通过 Engine API 操作，进程内或独立进程
class Editor {
public:
    void Initialize(Engine* engine);    // 绑定到 Engine 实例
    void OnFrame();                      // 编辑器帧（渲染 UI + 响应输入）
    void OnViewportResize(uint32_t w, uint32_t h);
    
    // 编辑操作 (通过 Command 模式，支持 Undo/Redo)
    void SelectEntity(Entity::ID id);
    void ModifyProperty(Entity::ID entity, meta::info property, const void* newValue);
    void Undo();
    void Redo();
    
    // PIE (Play In Editor)
    void Play();
    void Pause();
    void Stop();
    
private:
    Engine*                         m_Engine;
    // ImGui 面板
    WorldOutlinerPanel              m_Outliner;
    DetailsPanel                    m_DetailsPanel;
    ContentBrowserPanel             m_ContentBrowser;
    ViewportPanel                   m_Viewport;
    ConsolePanel                    m_Console;
    StatsPanel                      m_Stats;
    
    // Command 历史
    std::vector<std::unique_ptr<ICommand>> m_UndoStack;
    std::vector<std::unique_ptr<ICommand>> m_RedoStack;
};
```

---

## 7. 测试策略

### 7.1 测试金字塔

```
         ┌──────┐
         │ E2E  │  场景渲染截图对比 (每 Phase 2-3 个金图)
         │ 截图  │  - Phase 1: Cornell Box PBR
         ├──────┤  - Phase 3: Nanite 压力测试场景
         │ 集成  │  - Phase 4: GI + RT 综合场景
         │ 测试  │  模块交互测试 (glTF Load → Component → Render)
         ├──────┤  RHI 后端对比测试 (Vulkan vs D3D12 像素级一致)
         │ 单元  │  Math / Reflection / Serialization / SceneGraph
         │ 测试  │  Component 生命周期 / RenderGraph Pass 依赖
         └──────┘
```

### 7.2 各级测试明细

| 层级 | 工具 | 覆盖目标 | 频率 |
|------|------|----------|------|
| 单元测试 | Catch2 / doctest | Math, Reflection, Serialization, Component, SceneGraph, glTF Parser | 每次 commit |
| RHI 测试 | 自研 RHI Test Harness | 每个 RHI 接口: Create/Destroy/Barrier/Copy/PSO | 每次 PR |
| 组件测试 | 自研 + Catch2 | Entity Add/Remove/Query, Prefab Instantiate/Override | 每次 PR |
| 截图对比 | 自研 ScreenshotComparator | Phase 里程碑场景, CI 自动对比, PSNR > 40dB | 每夜构建 |
| 性能回归 | 自研 Benchmark Runner | Frame time, DrawCall count, VRAM usage | 每夜构建 |
| GPU 调试 | RenderDoc / PIX / Nsight | 所有截图对比失败时自动截帧 | 按需 |
| 着色器测试 | Slang Test Framework | 每个 .slang 包含 #if TEST 块验证编译 | 每次 commit |

### 7.3 参考测试场景

| Phase | 测试场景 | 验证点 |
|-------|----------|--------|
| P1 | Cornell Box (PBR) | Forward + Deferred + TAA 一致性 |
| P1 | Sponza (glTF) | glTF 完整加载 + 材质 + 光照 |
| P2 | 10K Instanced Teapots | GPU Driven Culling + ExecuteIndirect |
| P3 | Nanite Buddha (1B triangles) | Virtual Geometry 正确性 + 帧率 |
| P4 | Sun Temple (RT GI) | Lumen vs DDGI vs PathTracing 视觉对比 |
| P5 | DLSS/FSR Calibration | 超分质量 + 帧生成正确性 |
| P6 | Open World Landscape | 大气 + 体积 + 地形 + 植被 |
| P7 | Underwater Scene | 焦散 + 3DGS + 体积 |
| P8 | Full Stress Test | 混合场景: Mesh + GS + Nanite + RT + Caustics |

---

## 8. 风险与缓解

| # | 风险 | 影响 | 概率 | 缓解措施 |
|---|------|------|------|----------|
| 1 | C++26 反射编译器支持延迟 | 高 | 中 | P1 即集成 Clang 19+/MSVC 2026，维护 ClangTool 回退方案 |
| 2 | Slang Metal/WebGPU 后端不稳定 | 中 | 中 | Metal 延迟到 P6，WebGPU 延迟到 P8；先用 D3D12+Vulkan 验证 |
| 3 | RTX Mega Geometry / Wide BVH 硬件支持差异 | 低 | 低 | 使用标准 DXR/VulkanRT Fallback 路径 |
| 4 | Nanite 软件光栅化性能不足 | 高 | 低 | 参考 UE5 开源研究，优先保证 Mesh Shader 路径可用 |
| 5 | 单人/小团队工期膨胀 | 高 | 高 | Phase 间设定硬截止；每个 Phase 有 MVP 裁剪清单；核心优先于完整 |
| 6 | Vulkan/D3D12 行为差异导致像素不一致 | 中 | 中 | RHI 测试矩阵 + 截图对比 CI + Barrier 自动生成 |
| 7 | 神经渲染 API (DLSS/FSR) SDK 许可限制 | 低 | 低 | Streamline 抽象层隔离厂商 SDK，开源发布时排除厂商 DLL |
| 8 | glTF 扩展 (KHR_gaussian_splatting) 标准化拖延 | 低 | 低 | 先用 .ply/.splat 格式，扩展标准化后适配 |
| 9 | Shader 编译时间随 Pass 数量膨胀 | 中 | 中 | Phase 1 即建立 Shader 变体管理系统，Phase 5 引入 ASD |

---

## 附录 A: 最小可行产品 (MVP) 裁剪清单

当工期紧张时，以下特性可作为 **"Nice to Have"** 裁剪到后续版本：

| Phase | 可裁剪特性 | 理由 |
|-------|-----------|------|
| P1 | Metal 后端, WebGPU 后端 | 先 Windows (D3D12+Vulkan) 验证 |
| P2 | VK_EXT_device_generated_commands | ExecuteIndirect 够用 |
| P3 | Impostor System, Software VRS | 非核心路径 |
| P4 | VXGI/SDFGI, Reservoir Splatting | DDGI + Lumen 双 GI 方案已足够 |
| P5 | PSSR 2.0, Compute Graph Compiler, RTXCR | 厂商特定，先 DLSS+FSR+XeSS |
| P6 | Spectral Rendering, Neural Appearance Models | 非实时必需 |
| P7 | 4DGS, Beta Splatting, Volumetric Caustics | 静态 3DGS + 屏幕空间焦散即 MVP |
| P8 | VR/XR 3DGS, Reflex 2 | 单视口优化先行 |

---

## 附录 B: 人力配置建议

| 角色 | 人数 | 职责 |
|------|------|------|
| 渲染架构师 | 1 | 架构设计, RenderGraph, RHI 核心, Nanite, GI |
| 图形程序员 A | 1 | 着色器 (Slang), PBR, 后处理, 大气, 焦散 |
| 图形程序员 B | 1 | RT/ReSTIR, NRD, 神经渲染, 3DGS |
| 工具程序员 | 1 | 编辑器, Asset Pipeline, Material Editor, Visual Script |
| 引擎程序员 | 1 | Component, Reflection, Serialization, Job System, 动画 |

> 单人开发场景: 按 Phase 1→8 顺序执行，预计 3-4 年完成全部 MVP。

---

> **文档版本**: v1.0  
> **基于**: [HugEngine_Technical_Plan.md](HugEngine_Technical_Plan.md) v3.0  
> **总模块数**: 145 (EP 58 + ST ~200)  
> **关键路径**: 85 周 (可并行缓冲 34 周)
