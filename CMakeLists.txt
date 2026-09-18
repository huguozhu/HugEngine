cmake_minimum_required(VERSION 3.28)
project(HugEngine
    VERSION 0.1.0
    DESCRIPTION "Modern real-time rendering engine"
    LANGUAGES CXX
)

# VS 解决方案目录分组
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# 辅助函数：按磁盘目录结构分组源文件（.h 和 .cpp 合并到同一目录组）
# 替代 VS 默认的 "Header Files" / "Source Files" 分离
function(huge_source_group)
    foreach(_src ${ARGN})
        get_filename_component(_dir "${_src}" DIRECTORY)
        if(_dir)
            string(REPLACE "/" "\\" _group "${_dir}")
            source_group("${_group}" FILES "${_src}")
        else()
            source_group("" FILES "${_src}")
        endif()
    endforeach()
endfunction()

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
    # 自动检测本机最新版本的 Vulkan SDK（cmake/VulkanSDK.cmake），
    # 不写死版本路径、不下载；找不到则报错终止配置
    include(cmake/VulkanSDK.cmake)
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

    # 所有配置都生成 PDB（含 Release）。
    # 理由：崩溃处理器（Samples/06.GILab/CrashHandler.cpp）用 DbgHelp + exe 同目录的 .pdb
    # 解析调用栈；Release 默认不带调试信息，于是崩溃报告只有「模块 + 偏移 + (无符号信息)」，
    # 偶发崩溃（GI 文档 §10.1 第 2 项）根本无法定位。代价是构建产物变大、链接略慢。
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)
    # VS 生成器给 Release 预置了 GenerateDebugInformation=false，只设上面的编译期格式还不够，
    # 链接器必须显式开 /DEBUG，否则不产出可解析的 exe PDB（只剩编译期 PDB 引用）。
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE    "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG")
endif()

add_subdirectory(Engine)

# ============================================================
# Tests（doctest 单元测试，默认关闭）
# ============================================================
if(HUGENGINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif()

# ============================================================
# Samples
# ============================================================
if(HUGENGINE_BUILD_SAMPLES)
    add_subdirectory(Samples)
endif()

# ============================================================
# VS 解决方案目录 — 覆盖所有子模块的 FOLDER 设置
# ============================================================
# 注意：这里必须**列全**所有第三方 / 引擎 / 示例目标 —— 漏掉的目标会直接散落在
# 解决方案根目录（实测踩到：Jolt、06.GILab、HugEditor、HugEngineTests）。
# External 第三方库（覆盖 GLFW 自带的 "GLFW3" 等）
foreach(_ext glfw glm spdlog Taskflow stb meshoptimizer update_mappings imgui Jolt)
    if(TARGET ${_ext})
        set_target_properties(${_ext} PROPERTIES FOLDER "External")
    endif()
endforeach()

# Engine 模块（含 Engine/Physics 编译出的 HugEnginePhysics）
foreach(_eng HugEngineCore HugEngineRHI HugEngineReflect HugEngineShader HugEngineSerialize
            HugEngineScene HugEngineRender HugEngineAsset HugEngineEditor HugEngineAI
            HugEnginePhysics CompileShaders)
    if(TARGET ${_eng})
        set_target_properties(${_eng} PROPERTIES FOLDER "Engine")
    endif()
endforeach()

# Samples（含 06.GILab 与编辑器；编辑器目标名是 HugEditor）
foreach(_sample 01.Triangle 02.Cube 03.Sponza-Forward 04.Sponza-Deferred 06.GILab 07.AISamples HugEditor)
    if(TARGET ${_sample})
        set_target_properties(${_sample} PROPERTIES FOLDER "Samples")
    endif()
endforeach()

# 测试
if(TARGET HugEngineTests)
    set_target_properties(HugEngineTests PROPERTIES FOLDER "Tests")
endif()
