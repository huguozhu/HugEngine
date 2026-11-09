# ============================================================
# PatchDeps.cmake —— 对 vendored 第三方做**可重复、幂等**的配置期补丁
#
# 为什么需要：引擎的 GPU 共享结构体（cbuffer 镜像、静态网格顶点）按 MSVC 的布局写死了
# 偏移/尺寸断言，而 MSVC 下 glm 会把 vec/mat 当 aligned gentypes（16 字节对齐）。
# 这版 glm（0.9.5.3+）的对齐启用条件里带了 `GLM_LANG_CXXMS_FLAG`，于是 clang（鸿蒙 NDK）
# 下 vec3 只有 12 字节、vec4/mat 退化为 4 字节对齐 ⇒ 与 shader/Slang 布局不一致。
#
# 补丁内容：去掉该门限，让"有 alignof 的编译器"都启用对齐（MSVC 行为不变）。
# 幂等：已打过的文件不会再次修改；找不到目标文件/目标行时只提示，不报错。
# ============================================================

function(huge_patch_dependencies)
    set(_glm_setup "${CMAKE_SOURCE_DIR}/Engine/External/glm/glm/detail/setup.hpp")
    if(NOT EXISTS "${_glm_setup}")
        return()
    endif()

    file(READ "${_glm_setup}" _txt)
    set(_needle "#if GLM_HAS_ALIGNOF && (GLM_LANG & GLM_LANG_CXXMS_FLAG) && (defined(GLM_FORCE_ALIGNED_GENTYPES) || (GLM_CONFIG_SIMD == GLM_ENABLE))")
    string(FIND "${_txt}" "${_needle}" _pos)
    if(_pos EQUAL -1)
        # 要么已经打过补丁，要么 glm 版本变了：都无需处理
        return()
    endif()

    string(REPLACE "${_needle}"
        "#if GLM_HAS_ALIGNOF && (defined(GLM_FORCE_ALIGNED_GENTYPES) || (GLM_CONFIG_SIMD == GLM_ENABLE))"
        _patched "${_txt}")
    file(WRITE "${_glm_setup}" "${_patched}")
    message(STATUS "HugEngine: 已为多编译器布局一致性修补 glm（aligned gentypes 不再限定 MSVC）")
endfunction()
