# ============================================================
# VulkanSDK.cmake — 自动定位本机 Vulkan SDK（不下载、不联网）
#
# 行为：
#   1. 在 VULKAN_SDK_ROOT（默认 C:/VulkanSDK）下搜索所有形如
#      x.y.z.w 的版本目录，选择最新版本（按版本号数值比较）
#   2. 要求目录内容完整（include/vulkan/vulkan.h + Lib/vulkan-1.lib），
#      不完整的目录视为无效，不参与版本比较
#   3. 找到后写入 ENV{VULKAN_SDK}，供 find_package(Vulkan) 与
#      find_program(SLANGC) 使用
#   4. 清除缓存中指向旧版本 SDK 的路径，避免 SDK 升级后
#      缓存路径失效导致配置失败
#   5. 找不到任何可用 SDK 时 FATAL_ERROR，直接终止 CMake 配置
# ============================================================

# SDK 安装根目录（可在命令行用 -DVULKAN_SDK_ROOT=... 覆盖）
set(VULKAN_SDK_ROOT "C:/VulkanSDK" CACHE PATH "Vulkan SDK 安装根目录")

# 收集根目录下的所有子目录/文件，稍后按内容过滤
file(GLOB _he_vulkan_sdk_dirs LIST_DIRECTORIES true "${VULKAN_SDK_ROOT}/*")

# 遍历候选项，找出内容完整且版本号最新的 SDK 目录
set(_he_vulkan_sdk_latest "")
foreach(_dir ${_he_vulkan_sdk_dirs})
    get_filename_component(_name "${_dir}" NAME)
    # 版本目录名形如 1.4.357.0，且必须包含 Vulkan 头文件与导入库
    if(_name MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$"
       AND EXISTS "${_dir}/include/vulkan/vulkan.h"
       AND EXISTS "${_dir}/Lib/vulkan-1.lib")
        # 按版本号数值比较（VERSION_GREATER 逐段比较，避免 1.4.9 与 1.4.357 的字符串误判）
        if(_he_vulkan_sdk_latest STREQUAL "" OR _name VERSION_GREATER _he_vulkan_sdk_latest)
            set(_he_vulkan_sdk_latest "${_name}")
        endif()
    endif()
endforeach()

# 没有找到任何可用 SDK：明确报错并终止配置（不尝试下载）
if(_he_vulkan_sdk_latest STREQUAL "")
    message(FATAL_ERROR
        "未在 \"${VULKAN_SDK_ROOT}\" 下找到可用的 Vulkan SDK"
        "（需包含 include/vulkan/vulkan.h 与 Lib/vulkan-1.lib）。"
        "请先安装 Vulkan SDK：https://vulkan.lunarg.com/")
endif()

# 选定的 SDK 完整路径（本次配置实际使用的目标地址）
set(HE_VULKAN_SDK_DIR "${VULKAN_SDK_ROOT}/${_he_vulkan_sdk_latest}")

# 写入环境变量，后续 find_package(Vulkan) / find_program(SLANGC) 均依赖它
set(ENV{VULKAN_SDK} "${HE_VULKAN_SDK_DIR}")

# SDK 升级后缓存中可能残留旧版本路径；检测到指向其他目录时清除缓存项，
# 强制 find_package / find_program 按新的 ENV{VULKAN_SDK} 重新查找
foreach(_var Vulkan_INCLUDE_DIR Vulkan_LIBRARY
             Vulkan_GLSLC_EXECUTABLE Vulkan_GLSLANG_VALIDATOR_EXECUTABLE SLANGC)
    if(DEFINED ${_var} AND NOT "${${_var}}" MATCHES "^${HE_VULKAN_SDK_DIR}")
        unset(${_var} CACHE)
    endif()
endforeach()

message(STATUS "Vulkan SDK: ${_he_vulkan_sdk_latest} (${HE_VULKAN_SDK_DIR})")
