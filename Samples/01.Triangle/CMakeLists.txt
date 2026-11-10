# ============================================================
# 01.Triangle —— 场景与渲染在 01.Triangle.App.cpp（与鸿蒙共用同一份）
#
# 桌面入口也在同一个 .cpp 里，由 HE_DESKTOP_ENTRY 隔开：
#   · 桌面（本 target）：定义 HE_DESKTOP_ENTRY ⇒ 编出 main（配置 + DesktopHost）；
#   · 鸿蒙（entry）：不定义该宏 ⇒ 不含 main，入口在 Platforms/OHOS 的 AppMain.cpp。
# 主循环、输入、后端热切换都在 Platforms/Win32/DesktopHost。
# ============================================================
if(HUGENGINE_PLATFORM_OHOS)
    return()
endif()

add_executable(01.Triangle
    01.Triangle.App.cpp
    01.Triangle.App.h
)

# 只有桌面 exe 需要 main；鸿蒙 entry 编同一份 .cpp 但没有这个宏
target_compile_definitions(01.Triangle PRIVATE HE_DESKTOP_ENTRY=1)

target_link_libraries(01.Triangle PRIVATE
    HugEngineDesktopHost
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineEditor
)

# 依赖 CompileShaders 确保 .spv.h 等着色器头文件已生成
add_dependencies(01.Triangle CompileShaders)
