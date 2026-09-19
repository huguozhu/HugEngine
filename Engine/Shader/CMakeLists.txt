# Shader — Slang 编译管线 (L3)
# Phase 2: slangc → SPIR-V → embedded C header

add_library(HugEngineShader INTERFACE)

target_include_directories(HugEngineShader INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/Public
    ${CMAKE_CURRENT_SOURCE_DIR}/Shaders   # ShaderTypes.slang 等 C++/Slang 共享头文件
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/Shaders
)

target_link_libraries(HugEngineShader INTERFACE HugEngineRHI)

# ============================================================
# Shader compilation: Slang -> SPIR-V → embedded C header
# ============================================================

find_program(SLANGC
    NAMES slangc
    HINTS ENV VULKAN_SDK "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin"
)

if(SLANGC)
    message(STATUS "Shader compiler: ${SLANGC} (Slang)")

    set(SHADER_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/Shaders")
    set(SPV_OUT_DIR  "${CMAKE_CURRENT_BINARY_DIR}/Shaders")

    # 显式列出所有需编译的 shader（file(GLOB) 无法跟踪新增文件，故用显式列表）
    #
    # 目录按功能划分：Lighting / GBuffer / GI / RayTracing / PathTracing / ReSTIR /
    # PostProcess / AntiAliasing / Culling / AI / Utility / Particles。
    # 共享头（ShaderTypes.slang / common.slang / pbr_common.slang / Atmosphere.slang /
    # PT_Common.slang / RT_DDGI.slang / RT_HitCommon.slang）留在 Shaders/ 根目录：
    #   · 各目录中的 shader 以裸文件名 include 它们，靠 -I "${SHADER_DIR}" 解析
    #   · ShaderTypes.slang 还被 C++ 直接 include（Material.h / LightingPass.cpp），
    #     依赖 Shaders/ 在 target_include_directories 中
    # 故移动入口 shader 无需改写任何 include；但不要让共享头离开根目录。
    set(VERT_SLANG
        # Lighting
        "${SHADER_DIR}/Lighting/DeferredLighting.vert.slang"
        "${SHADER_DIR}/Lighting/PBR.vert.slang"
        "${SHADER_DIR}/Lighting/Shadow.vert.slang"
        "${SHADER_DIR}/Lighting/Skybox.vert.slang"
        # GBuffer
        "${SHADER_DIR}/GBuffer/GBuffer.vert.slang"
        # Decal（任务 24：GBuffer 投影贴花）
        "${SHADER_DIR}/Decal/DecalProject.vert.slang"
        # GI
        "${SHADER_DIR}/GI/IBL_BRDF_LUT.vert.slang"
        "${SHADER_DIR}/GI/IBL_Irradiance.vert.slang"
        "${SHADER_DIR}/GI/IBL_Prefilter.vert.slang"
        "${SHADER_DIR}/GI/RSM_Generate.vert.slang"
        "${SHADER_DIR}/GI/SSAO.vert.slang"
        "${SHADER_DIR}/GI/SSAO_Blur.vert.slang"
        # AntiAliasing
        "${SHADER_DIR}/AntiAliasing/TAA_Resolve.vert.slang"
        "${SHADER_DIR}/AntiAliasing/FXAA.vert.slang"
        # PostProcess
        "${SHADER_DIR}/PostProcess/ToneMap.vert.slang"
        # Utility
        "${SHADER_DIR}/Utility/Fullscreen.vert.slang"
        "${SHADER_DIR}/Utility/Triangle.vert.slang"
        # Particles
        "${SHADER_DIR}/Particles/ParticleRender.vert.slang"
        # Nanite（虚拟几何模块）——§14.8 任务 3 的绘制端最小顶点着色器（1×1 目标全屏三角）
        "${SHADER_DIR}/Nanite/Nanite_Raster.vert.slang"
    )
    set(FRAG_SLANG
        # Lighting
        "${SHADER_DIR}/Lighting/DeferredLighting.frag.slang"
        "${SHADER_DIR}/Lighting/PBR.frag.slang"
        "${SHADER_DIR}/Lighting/Shadow.frag.slang"
        "${SHADER_DIR}/Lighting/Skybox.frag.slang"
        "${SHADER_DIR}/Lighting/PhysicalSky.frag.slang"
        # GBuffer
        "${SHADER_DIR}/GBuffer/GBuffer.frag.slang"
        # Decal（任务 24：GBuffer 投影贴花）
        "${SHADER_DIR}/Decal/DecalProject.frag.slang"
        # GI
        "${SHADER_DIR}/GI/IBL_BRDF_LUT.frag.slang"
        "${SHADER_DIR}/GI/IBL_Irradiance.frag.slang"
        "${SHADER_DIR}/GI/IBL_Prefilter.frag.slang"
        "${SHADER_DIR}/GI/RSM_Generate.frag.slang"
        "${SHADER_DIR}/GI/SSAO.frag.slang"
        "${SHADER_DIR}/GI/SSAO_Blur.frag.slang"
        "${SHADER_DIR}/GI/GTAO.frag.slang"
        "${SHADER_DIR}/GI/SSGI.frag.slang"
        "${SHADER_DIR}/GI/SSR.frag.slang"
        "${SHADER_DIR}/GI/RSM_Indirect.frag.slang"
        # Lumen（虚拟化几何 GI）——骨架阶段的占位输出，步骤 20 起被 ScreenProbeGather 替换
        "${SHADER_DIR}/Lumen/Lumen_Skeleton.frag.slang"
        "${SHADER_DIR}/Lumen/Lumen_Irradiance.frag.slang"
        # AntiAliasing
        "${SHADER_DIR}/AntiAliasing/TAA_Resolve.frag.slang"
        "${SHADER_DIR}/AntiAliasing/FXAA.frag.slang"
        "${SHADER_DIR}/AntiAliasing/SMAA_EdgeDetect.frag.slang"
        "${SHADER_DIR}/AntiAliasing/SMAA_BlendWeight.frag.slang"
        "${SHADER_DIR}/AntiAliasing/SMAA_Neighborhood.frag.slang"
        # PostProcess
        "${SHADER_DIR}/PostProcess/ToneMap.frag.slang"
        "${SHADER_DIR}/PostProcess/Denoise.frag.slang"
        "${SHADER_DIR}/PostProcess/Denoise_Upscale.frag.slang"
        "${SHADER_DIR}/PostProcess/RT_DenoiseTemporal.frag.slang"
        "${SHADER_DIR}/PostProcess/GaussianBlur.frag.slang"
        "${SHADER_DIR}/PostProcess/BrightPass.frag.slang"
        "${SHADER_DIR}/PostProcess/BloomComposite.frag.slang"
        "${SHADER_DIR}/PostProcess/DOF_CoC.frag.slang"
        "${SHADER_DIR}/PostProcess/DOF_Composite.frag.slang"
        "${SHADER_DIR}/PostProcess/MotionBlur.frag.slang"
        "${SHADER_DIR}/PostProcess/ColorGrading.frag.slang"
        "${SHADER_DIR}/PostProcess/CameraEffects.frag.slang"
        # Utility
        "${SHADER_DIR}/Utility/FullscreenCopy.frag.slang"
        "${SHADER_DIR}/Utility/Triangle.frag.slang"
        "${SHADER_DIR}/Utility/TriangleMesh.frag.slang"
        # Particles
        "${SHADER_DIR}/Particles/ParticleRender.frag.slang"
        # Nanite（虚拟几何模块）——§14.8 任务 3 的绘制端：每光栅化一个簇把计数原子加一
        "${SHADER_DIR}/Nanite/Nanite_Raster.frag.slang"
    )
    set(COMP_SLANG
        # GI
        "${SHADER_DIR}/GI/DDGI.comp.slang"
        # PathTracing
        "${SHADER_DIR}/PathTracing/PT_Atrous.comp.slang"
        # PathTracing — ReSTIR DI（直接光重采样，非 GI；由 PathTracingPipeline 独占驱动）
        "${SHADER_DIR}/PathTracing/ReSTIR_Init.comp.slang"
        "${SHADER_DIR}/PathTracing/ReSTIR_Temporal.comp.slang"
        "${SHADER_DIR}/PathTracing/ReSTIR_Spatial.comp.slang"
        # Culling
        "${SHADER_DIR}/Culling/GPUCull.comp.slang"
        "${SHADER_DIR}/Culling/GPUCull_Phase1.comp.slang"
        "${SHADER_DIR}/Culling/GPUCull_TwoPhase.comp.slang"
        "${SHADER_DIR}/Culling/HiZDownsample.comp.slang"
        "${SHADER_DIR}/Culling/InstancedCull.comp.slang"
        "${SHADER_DIR}/Culling/PersistentCull.comp.slang"
        # PostProcess
        "${SHADER_DIR}/PostProcess/AutoExposure.comp.slang"
        # AI
        "${SHADER_DIR}/AI/AITensorScale.comp.slang"
        "${SHADER_DIR}/AI/AITensorAdd.comp.slang"
        "${SHADER_DIR}/AI/AITextureSample.comp.slang"
        "${SHADER_DIR}/AI/AITextureGen.comp.slang"
        # Lumen（虚拟化几何 GI）——步骤 8：逐 mesh 距离场（scatter/convert）；步骤 10：Global SDF；步骤 11：sphere tracing
        "${SHADER_DIR}/Lumen/SDF_MeshScatter.comp.slang"
        "${SHADER_DIR}/Lumen/SDF_MeshFlood.comp.slang"
        "${SHADER_DIR}/Lumen/SDF_MeshFloodSeeds.comp.slang"
        "${SHADER_DIR}/Lumen/SDF_MeshConvert.comp.slang"
        "${SHADER_DIR}/Lumen/SDF_GlobalBuild.comp.slang"
        "${SHADER_DIR}/Lumen/SDF_LayerProbe.comp.slang"
        "${SHADER_DIR}/Lumen/SDF_RayMarch.comp.slang"
        "${SHADER_DIR}/Lumen/SDF_RayMarchDetail.comp.slang"
        "${SHADER_DIR}/Lumen/SDF_DebugView.comp.slang"
        "${SHADER_DIR}/Lumen/SurfaceCache_PageCheck.comp.slang"
        "${SHADER_DIR}/Lumen/SurfaceCache_Capture.comp.slang"
        "${SHADER_DIR}/Lumen/SurfaceCache_Feedback.comp.slang"
        "${SHADER_DIR}/Lumen/ScreenProbe_Gather.comp.slang"
        "${SHADER_DIR}/Lumen/ScreenProbe_Trace.comp.slang"
        "${SHADER_DIR}/Lumen/Lumen_SurfaceCacheSample.comp.slang"
        "${SHADER_DIR}/Lumen/Lumen_ScreenProbe_SHProject.comp.slang"
        "${SHADER_DIR}/Lumen/ScreenProbe_Filter.comp.slang"
        "${SHADER_DIR}/Lumen/Lumen_ProbeIrradiance.comp.slang"
        "${SHADER_DIR}/Lumen/Lumen_FarFieldCompare.comp.slang"
        "${SHADER_DIR}/Lumen/Lumen_FarFieldMerge.comp.slang"
        # Utility
        "${SHADER_DIR}/Utility/WorkGraph_Entry.comp.slang"
        "${SHADER_DIR}/Utility/TestClear.comp.slang"
        # Particles
        "${SHADER_DIR}/Particles/ParticleInit.comp.slang"
        "${SHADER_DIR}/Particles/ParticleEmit.comp.slang"
        "${SHADER_DIR}/Particles/ParticleSimulate.comp.slang"
        "${SHADER_DIR}/Particles/ParticleCulling.comp.slang"
        "${SHADER_DIR}/Particles/ParticleSort.comp.slang"
        # Nanite（虚拟几何模块）——§14.8 任务 3：「假簇 → 间接命令 + 计数」的 compute
        "${SHADER_DIR}/Nanite/Nanite_Cull.comp.slang"
    )
    # Ray Tracing 着色器（.rgen / .rmiss / .rchit / .rahit / .rcall）
    # 命名约定: EffectName.rgen.slang (RayGen), EffectName.rmiss.slang (Miss),
    #           EffectName.rchit.slang (ClosestHit), EffectName.rahit.slang (AnyHit)
    set(RT_SLANG
        # RayTracing
        "${SHADER_DIR}/RayTracing/RT_Triangle.rgen.slang"
        "${SHADER_DIR}/RayTracing/RT_Background.rmiss.slang"
        "${SHADER_DIR}/RayTracing/RT_Sponza.rgen.slang"
        "${SHADER_DIR}/RayTracing/RT_Sponza.rmiss.slang"
        "${SHADER_DIR}/RayTracing/RT_Sponza.rchit.slang"
        "${SHADER_DIR}/RayTracing/RT_Bindless.rcall.slang"
        "${SHADER_DIR}/RayTracing/RT_Shadow.rgen.slang"
        "${SHADER_DIR}/RayTracing/RT_Shadow.rahit.slang"
        "${SHADER_DIR}/RayTracing/RT_Shadow.rmiss.slang"
        "${SHADER_DIR}/RayTracing/RT_Common.rmiss.slang"
        "${SHADER_DIR}/RayTracing/RT_Common.rchit.slang"
        "${SHADER_DIR}/RayTracing/RT_PBR.rchit.slang"
        "${SHADER_DIR}/RayTracing/RT_PositionFetch.rchit.slang"
        "${SHADER_DIR}/RayTracing/RT_AO.rgen.slang"
        "${SHADER_DIR}/RayTracing/RT_Reflection.rgen.slang"
        "${SHADER_DIR}/RayTracing/RT_Reflection.rchit.slang"
        "${SHADER_DIR}/RayTracing/RT_Reflection.rmiss.slang"
        "${SHADER_DIR}/RayTracing/RT_GI.rgen.slang"
        "${SHADER_DIR}/Lumen/Lumen_FarField.rgen.slang"
        "${SHADER_DIR}/RayTracing/RT_GI.rchit.slang"
        "${SHADER_DIR}/RayTracing/RT_GI.rmiss.slang"
        # DDGI 探针射线的光追 march（任务 17 / B4；复用 RT_GI 的 rchit/rmiss）
        "${SHADER_DIR}/RayTracing/DDGI_Trace.rgen.slang"
        # PathTracing
        "${SHADER_DIR}/PathTracing/PT_Full.rgen.slang"
        "${SHADER_DIR}/PathTracing/PT_Full.rchit.slang"
        "${SHADER_DIR}/PathTracing/PT_Full.rmiss.slang"
    )
    # Mesh Shader 着色器（.mesh / .task）
    # 命名约定: EffectName.mesh.slang (Mesh), EffectName.task.slang (Amplification/Task)
    set(MESH_SLANG
        "${SHADER_DIR}/GBuffer/GBuffer.mesh.slang"
        "${SHADER_DIR}/Utility/Triangle.mesh.slang"
    )
    # 公共共享头（被多个 shader include）——每个 shader 编译依赖这些；
    # 单个着色器文件本身由各自 SRC 依赖。改为显式列表而非 glob 所有 .slang，
    # 否则修改任一 shader（如 PBR.frag）会因 "DEPENDS 所有 shader" 触发全部重编译。
    set(SLANG_INCLUDES
        "${SHADER_DIR}/ShaderTypes.slang"
        "${SHADER_DIR}/common.slang"
        "${SHADER_DIR}/pbr_common.slang"
        "${SHADER_DIR}/Atmosphere.slang"
        "${SHADER_DIR}/PT_Common.slang"
        "${SHADER_DIR}/RT_DDGI.slang"
        "${SHADER_DIR}/RT_HitCommon.slang"
        "${SHADER_DIR}/Particles/ParticleTypes.slang"
        "${SHADER_DIR}/Lumen/SurfaceCache.slang"
        "${SHADER_DIR}/Lumen/ScreenProbe.slang"
    )

    set(COMPILE_SOURCES ${VERT_SLANG} ${FRAG_SLANG} ${COMP_SLANG} ${RT_SLANG} ${MESH_SLANG})
    set(SPV_FILES "")

    # 顶点着色器
    foreach(SRC ${VERT_SLANG})
        get_filename_component(FNAME ${SRC} NAME)      # PBR.vert.slang
        string(REGEX REPLACE "\\.slang$" "" NAME "${FNAME}")  # PBR.vert
        set(SPV "${SPV_OUT_DIR}/${NAME}.spv")
        add_custom_command(
            OUTPUT ${SPV}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SPV_OUT_DIR}
            COMMAND ${SLANGC} ${SRC} -target spirv -entry vertexMain -stage vertex
                    -I "${SHADER_DIR}" -o ${SPV} -Wno-39001
            DEPENDS ${SRC} ${SLANG_INCLUDES}
            COMMENT "Slang -> SPIR-V: ${NAME}"
        )
        list(APPEND SPV_FILES ${SPV})
    endforeach()

    # 片元着色器
    foreach(SRC ${FRAG_SLANG})
        get_filename_component(FNAME ${SRC} NAME)
        string(REGEX REPLACE "\\.slang$" "" NAME "${FNAME}")
        set(SPV "${SPV_OUT_DIR}/${NAME}.spv")
        add_custom_command(
            OUTPUT ${SPV}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SPV_OUT_DIR}
            COMMAND ${SLANGC} ${SRC} -target spirv -entry fragmentMain -stage fragment
                    -I "${SHADER_DIR}" -o ${SPV} -Wno-39001
            DEPENDS ${SRC} ${SLANG_INCLUDES}
            COMMENT "Slang -> SPIR-V: ${NAME}"
        )
        list(APPEND SPV_FILES ${SPV})
    endforeach()

    # 计算着色器（.comp.slang）
    foreach(SRC ${COMP_SLANG})
        get_filename_component(FNAME ${SRC} NAME)      # MyShader.comp.slang
        string(REGEX REPLACE "\\.slang$" "" NAME "${FNAME}")  # MyShader.comp
        set(SPV "${SPV_OUT_DIR}/${NAME}.spv")
        add_custom_command(
            OUTPUT ${SPV}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SPV_OUT_DIR}
            COMMAND ${SLANGC} ${SRC} -target spirv -entry main -stage compute
                    -I "${SHADER_DIR}" -o ${SPV} -Wno-39001
            DEPENDS ${SRC} ${SLANG_INCLUDES}
            COMMENT "Slang -> SPIR-V: ${NAME}"
        )
        list(APPEND SPV_FILES ${SPV})
    endforeach()

    # ── Ray Tracing 着色器 ──
    # Slang stage 映射: rgen→raygeneration, rmiss→miss, rchit→closesthit, rahit→anyhit, rcall→callable
    foreach(SRC ${RT_SLANG})
        get_filename_component(FNAME ${SRC} NAME)
        string(REGEX REPLACE "\\.slang$" "" NAME "${FNAME}")         # RT_Shadow.rgen
        string(REGEX MATCH "rgen|rmiss|rchit|rahit|rcall" RT_STAGE "${NAME}")
        set(SPV "${SPV_OUT_DIR}/${NAME}.spv")
        if(RT_STAGE STREQUAL "rgen")
            set(SLANGC_STAGE "raygeneration")
        elseif(RT_STAGE STREQUAL "rmiss")
            set(SLANGC_STAGE "miss")
        elseif(RT_STAGE STREQUAL "rchit")
            set(SLANGC_STAGE "closesthit")
        elseif(RT_STAGE STREQUAL "rahit")
            set(SLANGC_STAGE "anyhit")
        elseif(RT_STAGE STREQUAL "rcall")
            set(SLANGC_STAGE "callable")
        endif()
        add_custom_command(
            OUTPUT ${SPV}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SPV_OUT_DIR}
            COMMAND ${SLANGC} ${SRC} -target spirv -entry main -stage ${SLANGC_STAGE}
                    -I "${SHADER_DIR}" -o ${SPV} -Wno-39001
            DEPENDS ${SRC} ${SLANG_INCLUDES}
            COMMENT "Slang -> SPIR-V (RT): ${NAME} [${SLANGC_STAGE}]"
        )
        list(APPEND SPV_FILES ${SPV})
    endforeach()

    # ── Mesh Shader 着色器 ──
    # Slang stage 映射: mesh→mesh, task→task (Amplification Shader = Task Shader in Vulkan)
    foreach(SRC ${MESH_SLANG})
        get_filename_component(FNAME ${SRC} NAME)
        string(REGEX REPLACE "\\.slang$" "" NAME "${FNAME}")         # GBuffer.mesh
        string(REGEX MATCH "mesh|task" MS_STAGE "${NAME}")
        set(SPV "${SPV_OUT_DIR}/${NAME}.spv")
        if(MS_STAGE STREQUAL "mesh")
            set(SLANGC_STAGE "mesh")
        elseif(MS_STAGE STREQUAL "task")
            set(SLANGC_STAGE "task")
        endif()
        add_custom_command(
            OUTPUT ${SPV}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SPV_OUT_DIR}
            COMMAND ${SLANGC} ${SRC} -target spirv -entry main -stage ${SLANGC_STAGE}
                    -I "${SHADER_DIR}" -o ${SPV} -Wno-39001
            DEPENDS ${SRC} ${SLANG_INCLUDES}
            COMMENT "Slang -> SPIR-V (Mesh): ${NAME} [${SLANGC_STAGE}]"
        )
        list(APPEND SPV_FILES ${SPV})
    endforeach()

    # SPIR-V 二进制 → C++ uint32_t 数组（每个 .spv 独立生成 .spv.h，按需 include）
    set(SPV_HEADERS "")
    set(SPV_TOOL "${CMAKE_CURRENT_SOURCE_DIR}/spv_to_header.py")
    foreach(SPV ${SPV_FILES})
        get_filename_component(SPV_NAME ${SPV} NAME)
        set(SPV_H "${SPV_OUT_DIR}/${SPV_NAME}.h")
        list(APPEND SPV_HEADERS ${SPV_H})
        # 每个 spv 独立的 custom_command：只有变动时才重新生成对应 .h
        add_custom_command(
            OUTPUT ${SPV_H}
            COMMAND python "${SPV_TOOL}" "${SPV_OUT_DIR}" ${SPV}
            DEPENDS ${SPV} ${SPV_TOOL}
            COMMENT "SPV header: ${SPV_NAME}.h"
        )
    endforeach()

    # ── IDE 浏览：把 shader 源文件挂到 CompileShaders 项目 ──
    # 仅用于在 VS 解决方案中浏览/搜索，不参与编译（编译依赖仍由各 add_custom_command 的
    # DEPENDS 保证）。列表直接取自上面的显式编译列表 + 共享 include 列表，从而保证
    # 「IDE 里看到的 == 真正参与构建的」；若改用 GLOB，未列入编译的 .slang 也会显示出来，
    # 造成"看起来编了、其实没编"的错觉。
    # 转成相对路径，便于 huge_source_group 生成 Shaders / Shaders\Particles 目录树。
    set(SHADER_IDE_SOURCES "")
    foreach(_shader_src ${COMPILE_SOURCES} ${SLANG_INCLUDES})
        file(RELATIVE_PATH _shader_rel "${CMAKE_CURRENT_SOURCE_DIR}" "${_shader_src}")
        list(APPEND SHADER_IDE_SOURCES "${_shader_rel}")
    endforeach()
    list(REMOVE_DUPLICATES SHADER_IDE_SOURCES)

    add_custom_target(CompileShaders ALL
        DEPENDS ${SPV_HEADERS}
        SOURCES ${SHADER_IDE_SOURCES}   # IDE 视图：SOURCES 只影响工程文件，不构成构建依赖
    )
    huge_source_group(${SHADER_IDE_SOURCES})
    add_dependencies(HugEngineShader CompileShaders)

else()
    message(FATAL_ERROR "slangc NOT found — install Vulkan SDK: https://vulkan.lunarg.com/")
endif()
