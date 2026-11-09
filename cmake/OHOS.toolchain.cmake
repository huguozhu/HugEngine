# ============================================================
# OHOS.toolchain.cmake —— HarmonyOS NEXT 交叉编译入口（包装 NDK 自带的 toolchain）
#
# 用法：
#   cmake -S . -B build-ohos \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/OHOS.toolchain.cmake \
#         -DOHOS_NDK=<HarmonyOS SDK>/native \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-ohos -j
#
# 说明：
#   · OHOS_NDK 指向 DevEco Studio SDK 里的 native 目录（含 llvm/、sysroot/、build/cmake/）。
#   · 本工程只支持 arm64-v8a（手机），不带 x86 模拟器目标。
#   · 顶层 CMakeLists 通过 HUGENGINE_PLATFORM_OHOS 裁掉 D3D / DXC / GLFW / 编辑器 / 物理。
# ============================================================

# OHOS_NDK 既可以 -D 传入，也可以走环境变量。
# 【为什么需要环境变量兜底】CMake 的 try_compile 会另起一次 configure，
# 缓存变量传不进去；只有环境变量会被子进程继承，否则工具链文件会在
# 编译器 ABI 探测阶段就报"没传 OHOS_NDK"。
if(NOT DEFINED OHOS_NDK)
    if(DEFINED ENV{OHOS_NDK})
        set(OHOS_NDK "$ENV{OHOS_NDK}" CACHE PATH "HarmonyOS SDK native 目录")
    else()
        message(FATAL_ERROR "请传入 -DOHOS_NDK=<HarmonyOS SDK>/native，或设置环境变量 OHOS_NDK")
    endif()
endif()

# 只构建 arm64（手机）
set(OHOS_ARCH     "arm64-v8a" CACHE STRING "" FORCE)
set(OHOS_PLATFORM "ohos"      CACHE STRING "" FORCE)

# 交给 NDK 自带的 toolchain（设置 clang / sysroot / 目标三元组）
include("${OHOS_NDK}/build/cmake/ohos.toolchain.cmake")

# 顶层平台判定用（不同 SDK 版本的变量名不一致，这里统一兜底）
set(CMAKE_SYSTEM_NAME OHOS)
set(OHOS TRUE)
set(HUGENGINE_PLATFORM_OHOS ON)

message(STATUS "HugEngine: HarmonyOS toolchain, arch=${OHOS_ARCH}, ndk=${OHOS_NDK}")
