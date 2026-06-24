# Shader — Slang/GLSL compilation (L3)
# Phase 1 · Week 9-10

add_library(HugEngineShader INTERFACE)

target_include_directories(HugEngineShader INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/Public
    ${CMAKE_CURRENT_BINARY_DIR}          # For generated EmbeddedShaders.h (legacy)
    ${CMAKE_CURRENT_BINARY_DIR}/Shaders  # EmbeddedShaders.h 实际生成路径
)

target_link_libraries(HugEngineShader INTERFACE HugEngineRHI)

# ============================================================
# Shader compilation: GLSL → SPIR-V → embedded C header
# ============================================================

find_program(GLSLANG_VALIDATOR
    NAMES glslangValidator glslang
    HINTS ENV VULKAN_SDK "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin"
)

if(GLSLANG_VALIDATOR)
    message(STATUS "Shader compiler: ${GLSLANG_VALIDATOR}")

    set(SHADER_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/Shaders")
    set(SPV_OUT_DIR  "${CMAKE_CURRENT_BINARY_DIR}/Shaders")
    set(HEADER_FILE  "${SPV_OUT_DIR}/EmbeddedShaders.h")

    # Collect all .vert / .frag files
    file(GLOB VERT_SHADERS "${SHADER_DIR}/*.vert")
    file(GLOB FRAG_SHADERS "${SHADER_DIR}/*.frag")

    set(SPV_FILES "")

    # Compile vertex shaders
    foreach(SRC ${VERT_SHADERS})
        get_filename_component(NAME ${SRC} NAME_WE)
        set(SPV "${SPV_OUT_DIR}/${NAME}.vert.spv")
        add_custom_command(
            OUTPUT ${SPV}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SPV_OUT_DIR}
            COMMAND ${GLSLANG_VALIDATOR} -V -S vert ${SRC} -o ${SPV}
            DEPENDS ${SRC}
            COMMENT "SPIR-V: ${NAME}.vert"
        )
        list(APPEND SPV_FILES ${SPV})
    endforeach()

    # Compile fragment shaders
    foreach(SRC ${FRAG_SHADERS})
        get_filename_component(NAME ${SRC} NAME_WE)
        set(SPV "${SPV_OUT_DIR}/${NAME}.frag.spv")
        add_custom_command(
            OUTPUT ${SPV}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SPV_OUT_DIR}
            COMMAND ${GLSLANG_VALIDATOR} -V -S frag ${SRC} -o ${SPV}
            DEPENDS ${SRC}
            COMMENT "SPIR-V: ${NAME}.frag"
        )
        list(APPEND SPV_FILES ${SPV})
    endforeach()

    # 用 Python 脚本将 SPIR-V 二进制转为 C++ uint32_t 数组
    set(SPV_TOOL "${CMAKE_CURRENT_SOURCE_DIR}/spv_to_header.py")
    add_custom_command(
        OUTPUT ${HEADER_FILE}
        COMMAND python "${SPV_TOOL}" "${HEADER_FILE}" ${SPV_FILES}
        DEPENDS ${SPV_FILES} ${SPV_TOOL}
        COMMENT "Generating EmbeddedShaders.h (Python)"
    )

    # Target that all shader compilation feeds into
    add_custom_target(CompileShaders ALL DEPENDS ${HEADER_FILE})
    add_dependencies(HugEngineShader CompileShaders)

else()
    message(WARNING "glslangValidator NOT found — embedded SPIR-V fallback used")
    message(WARNING "Install Vulkan SDK: https://vulkan.lunarg.com/")

    # Provide a fallback header with a single hardcoded triangle shader
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Shaders")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/Shaders/EmbeddedShaders.h"
        "// Fallback: no glslangValidator available\n#pragma once\n#include <cstdint>\n#include <vector>\n"
        "// Hardcoded triangle vertex shader (vec2 pos → gl_Position)\n"
        "inline const std::vector<uint32_t> k_Triangle_vert_spv = {\n"
        "    0x07230203u,0x00010000u,0x0008000bu,0x00000018u,\n"
        "    0x00000000u,0x00020011u,0x00000001u,0x0006000bu,\n"
        "    0x00000001u,0x4c534c47u,0x6474732eu,0x3035342eu,\n"
        "    0x00000000u,0x0003000eu,0x00000000u,0x00000001u,\n"
        "    0x0007000fu,0x00000000u,0x00000004u,0x6e69616du,\n"
        "    0x00000000u,0x0000000du,0x0000000fu,0x00030003u,\n"
        "    0x00000002u,0x000001a4u,0x00040005u,0x00000004u,\n"
        "    0x6e69616du,0x00000000u,0x00060005u,0x0000000bu,\n"
        "    0x505f6c67u,0x65567265u,0x78657472u,0x00000000u,\n"
        "    0x00060006u,0x00000000u,0x00000000u,0x505f6c67u,\n"
        "    0x74697265u,0x65637875u,0x00030005u,0x0000000du,\n"
        "    0x6f506e69u,0x69746973u,0x00006e6fu,0x00050005u,\n"
        "    0x0000000fu,0x6f506e69u,0x69746973u,0x00006e6fu,\n"
        "    0x00040005u,0x00000011u,0x706f5f67u,0x00000073u,\n"
        "    0x00040047u,0x0000000du,0x0000001eu,0x00000000u,\n"
        "    0x00040047u,0x0000000fu,0x0000001eu,0x00000000u,\n"
        "    0x00020013u,0x00000002u,0x00030021u,0x00000003u,\n"
        "    0x00000002u,0x00030016u,0x00000006u,0x00000020u,\n"
        "    0x00040017u,0x00000007u,0x00000006u,0x00000002u,\n"
        "    0x00040017u,0x00000008u,0x00000006u,0x00000004u,\n"
        "    0x00040020u,0x0000000cu,0x00000001u,0x00000007u,\n"
        "    0x0004003bu,0x0000000cu,0x0000000du,0x00000001u,\n"
        "    0x00040020u,0x0000000eu,0x00000003u,0x00000008u,\n"
        "    0x0004003bu,0x0000000eu,0x0000000fu,0x00000003u,\n"
        "    0x00040015u,0x00000010u,0x00000020u,0x00000000u,\n"
        "    0x0004002bu,0x00000010u,0x00000011u,0x00000000u,\n"
        "    0x00020017u,0x00000012u,0x0004002bu,0x00000006u,\n"
        "    0x00000014u,0x00000000u,0x00020017u,0x00000015u,\n"
        "    0x0004002bu,0x00000006u,0x00000017u,0x3f800000u,\n"
        "    0x00050036u,0x00000002u,0x00000004u,0x00000000u,\n"
        "    0x00000003u,0x000200f8u,0x00000005u,0x0004003du,\n"
        "    0x00000007u,0x00000013u,0x0000000du,0x00050051u,\n"
        "    0x00000006u,0x00000016u,0x00000013u,0x00000000u,\n"
        "    0x00050051u,0x00000006u,0x00000018u,0x00000013u,\n"
        "    0x00000001u,0x00070050u,0x00000008u,0x00000019u,\n"
        "    0x00000016u,0x00000018u,0x00000014u,0x00000017u,\n"
        "    0x0003003eu,0x0000000fu,0x00000019u,0x000100fdu,\n"
        "    0x00010038u\n"
        "};\n\n"
        "// Hardcoded triangle fragment shader (solid red output)\n"
        "inline const std::vector<uint32_t> k_Triangle_frag_spv = {\n"
        "    0x07230203u,0x00010000u,0x0008000bu,0x00000014u,\n"
        "    0x00000000u,0x00020011u,0x00000001u,0x0006000bu,\n"
        "    0x00000001u,0x4c534c47u,0x6474732eu,0x3035342eu,\n"
        "    0x00000000u,0x0003000eu,0x00000000u,0x00000001u,\n"
        "    0x0007000fu,0x00000004u,0x00000004u,0x6e69616du,\n"
        "    0x00000000u,0x00000009u,0x0000000cu,0x00030010u,\n"
        "    0x00000004u,0x00000007u,0x00030003u,0x00000002u,\n"
        "    0x000001a4u,0x00040005u,0x00000004u,0x6e69616du,\n"
        "    0x00000000u,0x00050005u,0x00000009u,0x4374756fu,\n"
        "    0x726f6c6fu,0x00000000u,0x00060005u,0x0000000bu,\n"
        "    0x505f6c67u,0x65567265u,0x78657472u,0x00000000u,\n"
        "    0x00060006u,0x00000000u,0x00000000u,0x505f6c67u,\n"
        "    0x74697265u,0x65637875u,0x00030005u,0x0000000cu,\n"
        "    0x6f5f6c67u,0x00000070u,0x00040047u,0x00000009u,\n"
        "    0x0000001eu,0x00000000u,0x00040047u,0x0000000cu,\n"
        "    0x0000000bu,0x0000000fu,0x00020013u,0x00000002u,\n"
        "    0x00030021u,0x00000003u,0x00000002u,0x00030016u,\n"
        "    0x00000006u,0x00000020u,0x00040017u,0x00000007u,\n"
        "    0x00000006u,0x00000004u,0x00040020u,0x00000008u,\n"
        "    0x00000003u,0x00000007u,0x0004003bu,0x00000008u,\n"
        "    0x00000009u,0x00000003u,0x00040015u,0x0000000au,\n"
        "    0x00000020u,0x00000000u,0x0004002bu,0x0000000au,\n"
        "    0x0000000du,0x00000000u,0x00020017u,0x0000000eu,\n"
        "    0x0004002bu,0x00000006u,0x00000010u,0x3f800000u,\n"
        "    0x00020017u,0x00000011u,0x0004002bu,0x00000006u,\n"
        "    0x00000013u,0x3e99999au,0x00050036u,0x00000002u,\n"
        "    0x00000004u,0x00000000u,0x00000003u,0x000200f8u,\n"
        "    0x00000005u,0x00070050u,0x00000007u,0x0000000fu,\n"
        "    0x00000010u,0x00000010u,0x00000013u,0x00000010u,\n"
        "    0x0003003eu,0x00000009u,0x0000000fu,0x000100fdu,\n"
        "    0x00010038u\n"
        "};\n"
    )
endif()
