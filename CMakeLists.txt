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

# ============================================================
# Android 判定：NDK 工具链会把 CMAKE_SYSTEM_NAME 设为 Android；
# 也允许显式 -DHUGENGINE_PLATFORM_ANDROID=ON（例如在 Windows 上做语法自检）。
# 放在 project() 之后 —— ANDROID 变量由工具链在 project() 阶段注入。
# ============================================================
if(NOT DEFINED HUGENGINE_PLATFORM_ANDROID)
    if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Android")
        set(HUGENGINE_PLATFORM_ANDROID ON)
    else()
        set(HUGENGINE_PLATFORM_ANDROID OFF)
    endif()
endif()
# 移动平台公共标志：鸿蒙与 Android 都要裁掉 D3D/DXC/GLFW/桌面宿主/物理/AI
if(HUGENGINE_PLATFORM_OHOS OR HUGENGINE_PLATFORM_ANDROID)
    set(HUGENGINE_PLATFORM_MOBILE ON)
else()
    set(HUGENGINE_PLATFORM_MOBILE OFF)
endif()
message(STATUS "HugEngine: HUGENGINE_PLATFORM_ANDROID=${HUGENGINE_PLATFORM_ANDROID} MOBILE=${HUGENGINE_PLATFORM_MOBILE}")
# Android：NDK 的 libvulkan.so 不导出 Vulkan 1.3 的 vkGetDeviceBufferMemoryRequirements /
# vkGetDeviceImageMemoryRequirements，而 VMA 在 VMA_VULKAN_VERSION>=1.3 时才会用它们。
# 该宏必须对**所有** include vk_mem_alloc.h 的 TU 一致，因此全局定义而不是写在某个 .cpp 里。
if(HUGENGINE_PLATFORM_ANDROID)
    add_compile_definitions(VMA_VULKAN_VERSION=1002000)
endif()

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
# 【Android 例外】AGP 会通过工具链传入自己的输出目录（决定 .so 打进 APK 的位置），
# 这里覆盖会导致 APK 里找不到 libhgengine.so，所以 Android 构建不设这些变量。
if(NOT ANDROID)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
endif()

# ============================================================
# Options
# ============================================================
option(HUGENGINE_BUILD_SAMPLES   "Build sample projects" ON)
# 鸿蒙：构建 native 入口模块（libentry.so）；由 Platforms/OHOS 的 hvigor 工程传入 ON
option(HUGENGINE_BUILD_OHOS_ENTRY "Build HarmonyOS native entry (libentry.so)" OFF)
# Android：构建 native 入口模块（libhgengine.so）；由 Platforms/Android 的 Gradle 工程传入 ON
option(HUGENGINE_BUILD_ANDROID_ENTRY "Build Android native entry (libhgengine.so)" OFF)
if(HUGENGINE_PLATFORM_MOBILE)
    option(HUGENGINE_BUILD_EDITOR "Build editor" OFF)
else()
    option(HUGENGINE_BUILD_EDITOR "Build editor" ON)
endif()
option(HUGENGINE_BUILD_TESTS     "Build tests"           OFF)
option(HUGENGINE_RHI_VULKAN      "Enable Vulkan backend" ON)
# D3D 后端只在 Windows 上可用；非 Windows 平台即使打开也会被忽略
if(HUGENGINE_PLATFORM_MOBILE)
    option(HUGENGINE_RHI_D3D12   "Enable D3D12 backend (Windows only)" OFF)
    option(HUGENGINE_RHI_D3D11   "Enable D3D11 backend (Windows only)" OFF)
else()
    option(HUGENGINE_RHI_D3D12   "Enable D3D12 backend (Windows only)" ON)
    option(HUGENGINE_RHI_D3D11   "Enable D3D11 backend (Windows only)" ON)
endif()

# Vulkan：Windows 用本机 SDK；鸿蒙用 NDK sysroot 里的 libvulkan + 头文件；
# Android 用 NDK 的 libvulkan，但**头文件用随仓库的 1.4 头**（NDK r27 的 1.3 头缺少
# VK_EXT_device_generated_commands，引擎的 DGC 路径编不过）。
if(HUGENGINE_RHI_VULKAN)
    if(HUGENGINE_PLATFORM_MOBILE)
        find_library(HUGENGINE_VULKAN_LIB NAMES vulkan REQUIRED)
        if(HUGENGINE_PLATFORM_ANDROID)
            set(HUGENGINE_VULKAN_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/Engine/External/VulkanHeaders/include")
            message(STATUS "HugEngine: Android Vulkan -> ${HUGENGINE_VULKAN_LIB}（头文件用仓库内 1.4）")
        else()
            find_path(HUGENGINE_VULKAN_INCLUDE_DIR NAMES vulkan/vulkan.h REQUIRED)
            message(STATUS "HugEngine: OHOS Vulkan -> ${HUGENGINE_VULKAN_LIB}")
        endif()
        add_library(Vulkan::Vulkan UNKNOWN IMPORTED)
        set_target_properties(Vulkan::Vulkan PROPERTIES
            IMPORTED_LOCATION "${HUGENGINE_VULKAN_LIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${HUGENGINE_VULKAN_INCLUDE_DIR}")
    else()
        include(cmake/VulkanSDK.cmake)
        find_package(Vulkan REQUIRED)
    endif()
endif()

# Git（下载测试资源 + 子模块管理）；鸿蒙不下载资产（随 rawfile 打包）
if(NOT HUGENGINE_PLATFORM_MOBILE)
    find_package(Git REQUIRED)
endif()

# ============================================================
# 下载测试资源（Sponza glTF 等）
# ============================================================
if(NOT HUGENGINE_PLATFORM_MOBILE)
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
            HugEnginePhysics)
    if(TARGET ${_eng})
        set_target_properties(${_eng} PROPERTIES FOLDER "Engine")
    endif()
endforeach()

# Shader 编译相关目标统一收进 "CompileShader" 目录
#   CompileShaders*      ：slangc 批量编译（SPIR-V / DXIL / DXBC）
#   AuditShaderBindings  ：着色器绑定对账
#   ShaderToDxil*        ：HLSL 补丁工具与其单测（Tools/ShaderToDxil）
# 放在 Engine 循环之后，确保这里的归属覆盖前面可能设置过的 FOLDER。
foreach(_shader_target CompileShaders CompileShadersDxil CompileShadersDxbc AuditShaderBindings
                       ShaderToDxil ShaderToDxilTests)
    if(TARGET ${_shader_target})
        set_target_properties(${_shader_target} PROPERTIES FOLDER "CompileShader")
    endif()
endforeach()

# Samples（含 06.GILab / 07.Nanite 与编辑器；编辑器目标名是 HugEditor）
# 注意：这里必须列全，漏掉的目标会散落在解决方案根目录（07.Nanite 之前就漏了）
foreach(_sample 01.Triangle 02.Cube 03.Sponza-Forward 04.Sponza-Deferred 05.Sponza-PathTracing
                06.GILab 07.Nanite 08.AISamples HugEditor)
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

# Android native 入口（libhgengine.so）：由 Platforms/Android 的 Gradle 传 ON
if(HUGENGINE_BUILD_ANDROID_ENTRY)
    add_subdirectory(Platforms/Android/app/src/main/cpp)
endif()

# 鸿蒙侧源码的编译自检（仅桌面构建；只编不链接）
#   cmake --build build --target OhosStubCheck
# 用 stub 头 + HE_PLATFORM_OHOS_STUBCHECK 把鸿蒙分支真正编译一遍。
# ============================================================
if(NOT HUGENGINE_PLATFORM_MOBILE)
    add_subdirectory(Platforms/OHOS/stubcheck)
endif()