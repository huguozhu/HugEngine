cmake_minimum_required(VERSION 3.28)
project(HugEngine
    VERSION 0.1.0
    DESCRIPTION "Modern real-time rendering engine"
    LANGUAGES CXX
)

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

# ============================================================
# External dependencies (FetchContent — no vcpkg)
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
    add_compile_options(/Zc:preprocessor /bigobj /MP /utf-8 /Zc:noScanModuleDependencies)
endif()

add_subdirectory(Engine)

# ============================================================
# Samples
# ============================================================
if(HUGENGINE_BUILD_SAMPLES)
    add_subdirectory(Samples)
endif()
