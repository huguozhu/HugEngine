#pragma once

#include "Core/Platform.h"

#include <cstdio>
#include <cstdlib>

// ============================================================
// Assertion macros
// ============================================================

namespace he {

// Called when an assertion fails — can set a breakpoint here
HE_NO_INLINE
inline void AssertFailure(const char* expr, const char* file, int line, const char* msg = nullptr) {
    if (msg) {
        std::fprintf(stderr, "[ASSERT] %s:%d: %s — %s\n", file, line, expr, msg);
    } else {
        std::fprintf(stderr, "[ASSERT] %s:%d: %s\n", file, line, expr);
    }
    std::abort();
}

} // namespace he

// --- Assert macros ---
#ifdef HE_BUILD_DEBUG
    #define HE_ASSERT(expr, ...)                                        \
        do {                                                            \
            if (!(expr)) {                                              \
                he::AssertFailure(#expr, __FILE__, __LINE__, ##__VA_ARGS__); \
            }                                                           \
        } while (0)

    #define HE_ASSERT_MSG(expr, msg)                                    \
        do {                                                            \
            if (!(expr)) {                                              \
                he::AssertFailure(#expr, __FILE__, __LINE__, msg);      \
            }                                                           \
        } while (0)
#else
    #define HE_ASSERT(expr, ...)        ((void)0)
    #define HE_ASSERT_MSG(expr, msg)    ((void)0)
#endif

#define HE_UNREACHABLE()                                                \
    do {                                                                \
        he::AssertFailure("UNREACHABLE", __FILE__, __LINE__);           \
        __builtin_unreachable();                                        \
    } while (0)
