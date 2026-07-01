# Shader — Slang 编译管线 (L3)
# Phase 2: slangc → SPIR-V → embedded C header

add_library(HugEngineShader INTERFACE)

target_include_directories(HugEngineShader INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/Public
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

    # 收集所有 .slang 文件
    file(GLOB VERT_SLANG "${SHADER_DIR}/*.vert.slang")
    file(GLOB FRAG_SLANG "${SHADER_DIR}/*.frag.slang")
    file(GLOB SLANG_INCLUDES "${SHADER_DIR}/*.slang")  # 公共头文件

    # 过滤出非编译文件（仅 include）
    set(COMPILE_SOURCES ${VERT_SLANG} ${FRAG_SLANG})
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
