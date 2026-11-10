# ============================================================
# 平台判定：Windows（默认） / HarmonyOS NEXT（-DCMAKE_TOOLCHAIN_FILE=cmake/OHOS.toolchain.cmake）
# 鸿蒙构建把 D3D 后端、DXC/DXBC/DXIL 工具链、GLFW、编辑器、物理等全部裁掉，
# 只留 Vulkan + Core/Scene/Asset/Render 与 01/02 两个样例。
# ============================================================
if(CMAKE_SYSTEM_NAME STREQUAL "OHOS" OR OHOS OR DEFINED ENV{OHOS_NDK_ROOT})
    set(HUGENGINE_PLATFORM_OHOS ON)
else()
    set(HUGENGINE_PLATFORM_OHOS OFF)
endif()
message(STATUS "HugEngine: HUGENGINE_PLATFORM_OHOS=${HUGENGINE_PLATFORM_OHOS}")
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
# 鸿蒙：构建 native 入口模块（libentry.so）；由 Platforms/OHOS 的 hvigor 工程传入 ON
option(HUGENGINE_BUILD_OHOS_ENTRY "Build HarmonyOS native entry (libentry.so)" OFF)
if(HUGENGINE_PLATFORM_OHOS)
    option(HUGENGINE_BUILD_EDITOR "Build editor" OFF)
else()
    option(HUGENGINE_BUILD_EDITOR "Build editor" ON)
endif()
option(HUGENGINE_BUILD_TESTS     "Build tests"           OFF)
option(HUGENGINE_RHI_VULKAN      "Enable Vulkan backend" ON)
# D3D 后端只在 Windows 上可用；非 Windows 平台即使打开也会被忽略
if(HUGENGINE_PLATFORM_OHOS)
    option(HUGENGINE_RHI_D3D12   "Enable D3D12 backend (Windows only)" OFF)
    option(HUGENGINE_RHI_D3D11   "Enable D3D11 backend (Windows only)" OFF)
else()
    option(HUGENGINE_RHI_D3D12   "Enable D3D12 backend (Windows only)" ON)
    option(HUGENGINE_RHI_D3D11   "Enable D3D11 backend (Windows only)" ON)
endif()

# Vulkan：Windows 用本机 SDK；鸿蒙用 NDK sysroot 里的 libvulkan + 头文件
if(HUGENGINE_RHI_VULKAN)
    if(HUGENGINE_PLATFORM_OHOS)
        find_library(HUGENGINE_VULKAN_LIB NAMES vulkan REQUIRED)
        find_path(HUGENGINE_VULKAN_INCLUDE_DIR NAMES vulkan/vulkan.h REQUIRED)
        add_library(Vulkan::Vulkan UNKNOWN IMPORTED)
        set_target_properties(Vulkan::Vulkan PROPERTIES
            IMPORTED_LOCATION "${HUGENGINE_VULKAN_LIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${HUGENGINE_VULKAN_INCLUDE_DIR}")
        message(STATUS "HugEngine: OHOS Vulkan -> ${HUGENGINE_VULKAN_LIB}")
    else()
        include(cmake/VulkanSDK.cmake)
        find_package(Vulkan REQUIRED)
    endif()
endif()

# Git（下载测试资源 + 子模块管理）；鸿蒙不下载资产（随 rawfile 打包）
if(NOT HUGENGINE_PLATFORM_OHOS)
    find_package(Git REQUIRED)
endif()

# ============================================================
# 下载测试资源（Sponza glTF 等）
# ============================================================
if(NOT HUGENGINE_PLATFORM_OHOS)
    include(cmake/DownloadAssets.cmake)
endif()

# ============================================================
# External dependencies
# NOTE: External deps processed BEFORE C++26 to avoid
#       propagating CXX_STANDARD to sub-builds
# ============================================================
# vendored 第三方的可重复补丁（glm 对齐门限）
include(cmake/PatchDeps.cmake)
huge_patch_dependencies()

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
    # 理由：崩溃处理器（Engine/Core/Core/CrashHandler.cpp）用 DbgHelp + exe 同目录的 .pdb
    # 解析调用栈；Release 默认不带调试信息，于是崩溃报告只有「模块 + 偏移 + (无符号信息)」，
    # 偶发崩溃（GI 文档 §10.1 第 2 项）根本无法定位。代价是构建产物变大、链接略慢。
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)
    # VS 生成器给 Release 预置了 GenerateDebugInformation=false，只设上面的编译期格式还不够，
    # 链接器必须显式开 /DEBUG，否则不产出可解析的 exe PDB（只剩编译期 PDB 引用）。
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE    "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG")
endif()

# 工具链：D3D12 的着色器构建需要 ShaderToDxil（HLSL 补丁 + dxc）。
# 必须在 add_subdirectory(Engine) 之前定义，否则 Engine/Shader 里的
# 「if(TARGET ShaderToDxil)」判断为假，DXIL 着色器不会被生成。
if(WIN32)
    add_subdirectory(Tools/ShaderToDxil)
    # D3D12 Agility SDK：下载运行时并提供 huge_enable_d3d12_agility()（样例 exe 调用）
    include(cmake/AgilitySDK.cmake)
endif()

add_subdirectory(Engine)

# 桌面样例宿主：样例的 main 只做配置，主循环在 Platforms/Win32
if(WIN32 AND HUGENGINE_BUILD_SAMPLES)
    add_subdirectory(Platforms/Win32)
endif()

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
foreach(_sample 01.Triangle 02.Cube 03.Sponza-Forward 04.Sponza-Deferred 05.Sponza-PathTracing 06.GILab 08.AISamples HugEditor)
    if(TARGET ${_sample})
        set_target_properties(${_sample} PROPERTIES FOLDER "Samples")
    endif()
endforeach()

# 测试
if(TARGET HugEngineTests)
    set_target_properties(HugEngineTests PROPERTIES FOLDER "Tests")
endif()

# ============================================================
if(HUGENGINE_BUILD_OHOS_ENTRY)
    add_subdirectory(Platforms/OHOS/entry/src/main/cpp)
endif()

# 鸿蒙侧源码的编译自检（仅桌面构建；只编不链接）
#   cmake --build build --target OhosStubCheck
# 用 stub 头 + HE_PLATFORM_OHOS_STUBCHECK 把鸿蒙分支真正编译一遍。
# ============================================================
if(NOT HUGENGINE_PLATFORM_OHOS)
    add_subdirectory(Platforms/OHOS/stubcheck)
endif()