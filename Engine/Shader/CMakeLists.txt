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
# Shader compilation: Slang → SPIR-V → embedded C header
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
    set(VERT_SLANG
        "${SHADER_DIR}/DeferredLighting.vert.slang"
        "${SHADER_DIR}/FXAA.vert.slang"
        "${SHADER_DIR}/GBuffer.vert.slang"
        "${SHADER_DIR}/IBL_BRDF_LUT.vert.slang"
        "${SHADER_DIR}/IBL_Irradiance.vert.slang"
        "${SHADER_DIR}/IBL_Prefilter.vert.slang"
        "${SHADER_DIR}/PBR.vert.slang"
        "${SHADER_DIR}/RSM_Generate.vert.slang"
        "${SHADER_DIR}/Shadow.vert.slang"
        "${SHADER_DIR}/Skybox.vert.slang"
        "${SHADER_DIR}/SSAO.vert.slang"
        "${SHADER_DIR}/SSAO_Blur.vert.slang"
        "${SHADER_DIR}/TAA_Resolve.vert.slang"
        "${SHADER_DIR}/ToneMap.vert.slang"
        "${SHADER_DIR}/Triangle.vert.slang"
        "${SHADER_DIR}/Fullscreen.vert.slang"
    )
    set(FRAG_SLANG
        "${SHADER_DIR}/DeferredLighting.frag.slang"
        "${SHADER_DIR}/GBuffer.frag.slang"
        "${SHADER_DIR}/IBL_BRDF_LUT.frag.slang"
        "${SHADER_DIR}/IBL_Irradiance.frag.slang"
        "${SHADER_DIR}/IBL_Prefilter.frag.slang"
        "${SHADER_DIR}/PBR.frag.slang"
        "${SHADER_DIR}/RSM_Generate.frag.slang"
        "${SHADER_DIR}/Shadow.frag.slang"
        "${SHADER_DIR}/Skybox.frag.slang"
        "${SHADER_DIR}/SSAO.frag.slang"
        "${SHADER_DIR}/SSAO_Blur.frag.slang"
        "${SHADER_DIR}/SSGI.frag.slang"
        "${SHADER_DIR}/SSR.frag.slang"
        "${SHADER_DIR}/Denoise.frag.slang"
        "${SHADER_DIR}/TAA_Resolve.frag.slang"
        "${SHADER_DIR}/ToneMap.frag.slang"
        "${SHADER_DIR}/Triangle.frag.slang"
        "${SHADER_DIR}/FXAA.frag.slang"
        "${SHADER_DIR}/GaussianBlur.frag.slang"
        "${SHADER_DIR}/BrightPass.frag.slang"
        "${SHADER_DIR}/BloomComposite.frag.slang"
        "${SHADER_DIR}/DOF_CoC.frag.slang"
        "${SHADER_DIR}/DOF_Composite.frag.slang"
        "${SHADER_DIR}/MotionBlur.frag.slang"
        "${SHADER_DIR}/ColorGrading.frag.slang"
        "${SHADER_DIR}/SMAA_EdgeDetect.frag.slang"
        "${SHADER_DIR}/SMAA_BlendWeight.frag.slang"
        "${SHADER_DIR}/SMAA_Neighborhood.frag.slang"
        "${SHADER_DIR}/FullscreenCopy.frag.slang"
        "${SHADER_DIR}/TriangleMesh.frag.slang"
    )
    set(COMP_SLANG
        "${SHADER_DIR}/GPUCull.comp.slang"
        "${SHADER_DIR}/HiZDownsample.comp.slang"
        "${SHADER_DIR}/TestClear.comp.slang"
        "${SHADER_DIR}/DDGI.comp.slang"
        "${SHADER_DIR}/AutoExposure.comp.slang"
    )
    # Ray Tracing 着色器（.rgen / .rmiss / .rchit / .rahit / .rcall）
    # 命名约定: EffectName.rgen.slang (RayGen), EffectName.rmiss.slang (Miss),
    #           EffectName.rchit.slang (ClosestHit), EffectName.rahit.slang (AnyHit)
    set(RT_SLANG
        "${SHADER_DIR}/RT_Triangle.rgen.slang"
        "${SHADER_DIR}/RT_Background.rmiss.slang"
        "${SHADER_DIR}/RT_Sponza.rgen.slang"
        "${SHADER_DIR}/RT_Sponza.rmiss.slang"
        "${SHADER_DIR}/RT_Sponza.rchit.slang"
        "${SHADER_DIR}/RT_Bindless.rcall.slang"
        "${SHADER_DIR}/RT_Shadow.rgen.slang"
        "${SHADER_DIR}/RT_Common.rmiss.slang"
        "${SHADER_DIR}/RT_Common.rchit.slang"
    )
    # Mesh Shader 着色器（.mesh / .task）
    # 命名约定: EffectName.mesh.slang (Mesh), EffectName.task.slang (Amplification/Task)
    set(MESH_SLANG
        "${SHADER_DIR}/Triangle.mesh.slang"
        "${SHADER_DIR}/GBuffer.mesh.slang"
    )
    file(GLOB SLANG_INCLUDES "${SHADER_DIR}/*.slang")  # 公共头文件（用 glob 自动收集即可）

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
            COMMENT "Slang → SPIR-V: ${NAME}"
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
            COMMENT "Slang → SPIR-V: ${NAME}"
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
            COMMENT "Slang → SPIR-V: ${NAME}"
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
            COMMENT "Slang → SPIR-V (RT): ${NAME} [${SLANGC_STAGE}]"
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
            COMMENT "Slang → SPIR-V (Mesh): ${NAME} [${SLANGC_STAGE}]"
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

    add_custom_target(CompileShaders ALL DEPENDS ${SPV_HEADERS})
    add_dependencies(HugEngineShader CompileShaders)

else()
    message(FATAL_ERROR "slangc NOT found — install Vulkan SDK: https://vulkan.lunarg.com/")
endif()
