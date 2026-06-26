cmake_minimum_required(VERSION 3.28)
project(HugEngine
    VERSION 0.1.0
    DESCRIPTION "Modern real-time rendering engine"
    LANGUAGES CXX
)

# VS 解决方案目录分组
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ============================================================
# Options
# ============================================================
option(HUGENGINE_BUILD_SAMPLES   "Build sample projects" ON)
option(HUGENGINE_BUILD_EDITOR    "Build editor"          ON)
option(HUGENGINE_BUILD_TESTS     "Build tests"           OFF)
option(HUGENGINE_RHI_VULKAN      "Enable Vulkan backend" ON)

# Vulkan SDK（ImGui Vulkan 后端 + RHI 均需要）
if(HUGENGINE_RHI_VULKAN)
    find_package(Vulkan REQUIRED)
endif()

# Git（用于下载测试资源 + 子模块管理）
find_package(Git REQUIRED)

# ============================================================
# 下载测试资源（Sponza glTF 等）
# ============================================================
include(cmake/DownloadAssets.cmake)

# ============================================================
# External dependencies
# NOTE: External deps processed BEFORE C++26 to avoid
#       propagating CXX_STANDARD to sub-builds
# ============================================================
add_subdirectory(Engine/External)

# ============================================================
# Engine (C++20 applies only to HugEngine targets)
# MSVC: /std:c++20 /Zc:preprocessor
# ============================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/Zc:preprocessor /bigobj /MP /utf-8 )
    add_compile_definitions(NOMINMAX)
endif()

add_subdirectory(Engine)

# ============================================================
# Samples
# ============================================================
if(HUGENGINE_BUILD_SAMPLES)
    add_subdirectory(Samples)
endif()

# ============================================================
# VS 解决方案目录 — 覆盖所有子模块的 FOLDER 设置
# ============================================================
# External 第三方库（覆盖 GLFW 自带的 "GLFW3" 等）
foreach(_ext glfw glm spdlog Taskflow stb meshoptimizer update_mappings imgui)
    if(TARGET ${_ext})
        set_target_properties(${_ext} PROPERTIES FOLDER "External")
    endif()
endforeach()

# Engine 模块
foreach(_eng HugEngineCore HugEngineRHI HugEngineReflect HugEngineShader HugEngineSerialize
            HugEngineScene HugEngineRender HugEngineAsset HugEngineEditor CompileShaders)
    if(TARGET ${_eng})
        set_target_properties(${_eng} PROPERTIES FOLDER "Engine")
    endif()
endforeach()

# Samples
if(TARGET 01.Triangle)
    set_target_properties(01.Triangle PROPERTIES FOLDER "Samples")
endif()
if(TARGET 02.Cube)
    set_target_properties(02.Cube PROPERTIES FOLDER "Samples")
endif()
if(TARGET 03.Sponza)
    set_target_properties(03.Sponza PROPERTIES FOLDER "Samples")
endif()
