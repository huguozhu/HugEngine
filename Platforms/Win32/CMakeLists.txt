# ============================================================
# Platforms/Win32 —— 桌面样例宿主（平台相关代码的唯一落点）
#
# 例子：Samples/01.Triangle/Main.Win32.cpp 只做
#   cfg 配置 + he::samples::desktop::RunApp<App>(cfg)
#
# 鸿蒙对应物在 Platforms/OHOS/entry（XComponent + napi），两边的 App 是同一份。
# ============================================================

add_library(HugEngineDesktopHost STATIC
    DesktopHost.cpp
    DesktopHost.h
)

target_include_directories(HugEngineDesktopHost PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Samples/Common)

target_link_libraries(HugEngineDesktopHost PUBLIC
    HugEngineCore
    HugEngineRHI)

set_target_properties(HugEngineDesktopHost PROPERTIES
    FOLDER "Platforms/Win32")
