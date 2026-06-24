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

# --- meshoptimizer v0.22 ---
add_subdirectory(meshoptimizer)

# --- Catch2 (tests only) ---
if(HUGENGINE_BUILD_TESTS)
    add_subdirectory(Catch2)
endif()

message(STATUS "HugEngine: all external dependencies resolved (submodules)")
