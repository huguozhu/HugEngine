# ============================================================
# External dependencies — git submodules (无网络依赖)
# 初始化: git submodule update --init --recursive
# ============================================================

# 抑制第三方库 CMake 版本过低的警告
set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS ON CACHE BOOL "")

# --- GLFW 3.3.9 ---
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
add_subdirectory(glfw)

# --- GLM 1.0.1 (header-only) ---
set(GLM_TEST_ENABLE OFF CACHE BOOL "" FORCE)
add_subdirectory(glm)

# --- spdlog v1.14.1 ---
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL      OFF CACHE BOOL "" FORCE)
add_subdirectory(spdlog)

# --- Taskflow v3.9.0 (header-only) ---
set(TF_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TF_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
add_subdirectory(taskflow)

# --- stb (header-only, 无 CMake 构建) ---
if(NOT TARGET stb)
    add_library(stb INTERFACE)
    target_include_directories(stb INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/stb)
endif()

# --- nlohmann/json (header-only, 本地 vendor 零网络，源自 taskflow 3rd-party) ---
# 头文件位于 Engine/External/nlohmann/json.hpp，include 根为 Engine/External（"nlohmann/json.hpp"）
if(NOT TARGET nlohmann_json)
    add_library(nlohmann_json INTERFACE)
    target_include_directories(nlohmann_json INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# --- ImGui (header+source, 非 submodule — CDN 下载的源文件) ---
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw Vulkan::Vulkan)

# --- meshoptimizer v0.22 ---
add_subdirectory(meshoptimizer)

# --- Jolt Physics（刚体物理引擎，submodule；根 CMake 在 Build/ 子目录）---
# 集成指南：https://github.com/jrouwe/JoltPhysics/blob/master/Build/CMakeLists.txt
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)                    # 不生成安装目标
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)                # 跟随工程全局 flags
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)   # 用动态 CRT（/MDd），与工程链接一致
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)                      # 不需要 Jolt 自带图形渲染后端
set(JPH_USE_VK   OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL  OFF CACHE BOOL "" FORCE)
add_subdirectory(JoltPhysics/Build)
# Jolt 库目标名固定为 "Jolt"（Build/Jolt/CMakeLists.txt）

# --- 单元测试框架：doctest（header-only，本地 vendor，见 Engine/External/doctest）---
# 注：原 Catch2 子模块未初始化，改用 doctest 单头文件（源自 taskflow 3rd-party，零网络）
# if(HUGENGINE_BUILD_TESTS)
#     add_subdirectory(Catch2)
# endif()

message(STATUS "HugEngine: all external dependencies resolved (submodules)")
