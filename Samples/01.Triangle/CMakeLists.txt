# ============================================================
# 01.Triangle —— 场景与渲染在 01.Triangle.App.cpp（与鸿蒙共用同一份）
#
# 桌面入口 Main.Win32.cpp 只做配置 + 调用 Platforms/Win32 的 DesktopHost；
# 主循环、输入、后端热切换都在宿主里。鸿蒙侧由 Platforms/OHOS/entry 编同一个 App。
# ============================================================
if(HUGENGINE_PLATFORM_OHOS)
    return()
endif()

add_executable(01.Triangle
    Main.Win32.cpp
    01.Triangle.App.cpp
    01.Triangle.App.h
)

target_link_libraries(01.Triangle PRIVATE
    HugEngineDesktopHost
    HugEngineCore
    HugEngineRHI
    HugEngineShader
    HugEngineEditor
)

# 依赖 CompileShaders 确保 .spv.h 等着色器头文件已生成
add_dependencies(01.Triangle CompileShaders)
