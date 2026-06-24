# ============================================================
# External dependencies — GitHub release archives (no git clone)
# ============================================================

include(FetchContent)
cmake_policy(SET CMP0167 OLD) # CMake 4.x FetchContent 兼容

# 抑制第三方库 CMake 版本过低的警告
set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS ON CACHE BOOL "")

# Taskflow v3.9.0 需要 C++17 兼容模式（MSVC 2026 默认标准会移除 chrono::system_clock）
set(CMAKE_CXX_STANDARD 17 CACHE STRING "")
set(CMAKE_CXX_STANDARD_REQUIRED ON CACHE BOOL "")
set(CMAKE_CXX_EXTENSIONS OFF CACHE BOOL "")

# --- GLFW 3.4 ---
FetchContent_Declare(glfw
    URL https://github.com/glfw/glfw/archive/refs/tags/3.3.9.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)

# --- GLM 1.0.1 (header-only) ---
FetchContent_Declare(glm
    URL https://github.com/g-truc/glm/archive/refs/tags/1.0.1.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(GLM_TEST_ENABLE OFF CACHE BOOL "" FORCE)

# --- spdlog v1.14.1 ---
FetchContent_Declare(spdlog
    URL https://github.com/gabime/spdlog/archive/refs/tags/v1.14.1.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL      OFF CACHE BOOL "" FORCE)

# --- Taskflow v3.9.0 (header-only) ---
FetchContent_Declare(taskflow
    URL https://github.com/taskflow/taskflow/archive/refs/tags/v3.9.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(TF_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TF_BUILD_TESTS    OFF CACHE BOOL "" FORCE)

# --- stb (header-only, latest) ---
FetchContent_Declare(stb
    URL https://github.com/nothings/stb/archive/refs/heads/master.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# --- meshoptimizer v0.22 ---
FetchContent_Declare(meshoptimizer
    URL https://github.com/zeux/meshoptimizer/archive/refs/tags/v0.22.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# --- Catch2 (tests only) ---
if(HUGENGINE_BUILD_TESTS)
    FetchContent_Declare(Catch2
        URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.7.1.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
endif()

# ============================================================
# Make available all at once
# ============================================================
FetchContent_MakeAvailable(
    glfw
    glm
    spdlog
    taskflow
    stb
    meshoptimizer
)

# Taskflow v3.9.0 为 header-only 库，C++ 标准由消费方决定
# 不再覆盖其 CXX_STANDARD，避免模板实例化不一致

if(HUGENGINE_BUILD_TESTS)
    FetchContent_MakeAvailable(Catch2)
endif()

message(STATUS "HugEngine: all external dependencies resolved")
