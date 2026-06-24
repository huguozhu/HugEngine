#pragma once

// ============================================================
// Platform detection macros
// ============================================================

#if defined(_WIN32) || defined(_WIN64)
    #define HE_PLATFORM_WINDOWS 1
    #if defined(_WIN64)
        #define HE_PLATFORM_WINDOWS_64 1
    #else
        #define HE_PLATFORM_WINDOWS_32 1
    #endif
#elif defined(__APPLE__)
    #include <TargetConditionals.h>
    #if TARGET_OS_IPHONE
        #define HE_PLATFORM_IOS 1
    #elif TARGET_OS_MAC
        #define HE_PLATFORM_MACOS 1
    #endif
#elif defined(__linux__)
    #define HE_PLATFORM_LINUX 1
#endif

// --- Compiler detection ---
#if defined(_MSC_VER)
    #define HE_COMPILER_MSVC 1
    #define HE_FORCE_INLINE __forceinline
    #define HE_NO_INLINE    __declspec(noinline)
    #define HE_ALIGN(n)     __declspec(align(n))
#elif defined(__clang__)
    #define HE_COMPILER_CLANG 1
    #define HE_FORCE_INLINE __attribute__((always_inline)) inline
    #define HE_NO_INLINE    __attribute__((noinline))
    #define HE_ALIGN(n)     __attribute__((aligned(n)))
#elif defined(__GNUC__)
    #define HE_COMPILER_GCC 1
    #define HE_FORCE_INLINE __attribute__((always_inline)) inline
    #define HE_NO_INLINE    __attribute__((noinline))
    #define HE_ALIGN(n)     __attribute__((aligned(n)))
#endif

// --- Architecture ---
#if defined(__x86_64__) || defined(_M_X64)
    #define HE_ARCH_X64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define HE_ARCH_ARM64 1
#endif

// --- Debug / Release ---
#if defined(NDEBUG) || defined(_NDEBUG)
    #define HE_BUILD_RELEASE 1
#else
    #define HE_BUILD_DEBUG 1
#endif

// --- DLL export ---
#if HE_PLATFORM_WINDOWS
    #ifdef HE_BUILD_DLL
        #define HE_API __declspec(dllexport)
    #else
        #define HE_API __declspec(dllimport)
    #endif
#else
    #define HE_API
#endif

// Static builds — no dllimport needed
#ifdef HE_STATIC
    #undef HE_API
    #define HE_API
#endif
