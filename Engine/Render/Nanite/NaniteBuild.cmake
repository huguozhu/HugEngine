# ============================================================
#  Engine/Render/Nanite/NaniteBuild.cmake
#
#  T1（《01.Nanite离线预处理与全Mesh管线方案》）：把离线几何构建的**纯函数**原地
#  包成静态库 `HugEngineNaniteBuild`，供三处共用：
#    · `HugEngineRender`（运行时回退路径）；
#    · `Tools/MeshPreprocess`（离线工具，T3）；
#    · `HugEngineTests`（单测直接链库，不再重复编译源文件）。
#
#  设计约束（与方案 §5.1 一致）：
#   · **文件原地不动**：源文件仍在 Engine/Render/Nanite/，本文件只做目标定义；
#   · 本库**不链接任何 RHI 目标** —— 这正是"RHI 依赖不得闯入"的纪律钉子：
#     谁在 NaniteUpload.cpp / NaniteBuild 相关文件里 include 了 RHI 头，这里最先编译失败；
#   · meshoptimizer 的依赖随之从 HugEngineRender 移到这里（PRIVATE：它的头只出现在 .cpp）；
#   · GPU 上传（建缓冲/上传/读回）不在这里，仍在 `NaniteScene::UploadPackedAsset`。
# ============================================================

add_library(HugEngineNaniteBuild STATIC
    # 离线簇切分 / DAG / 量化打包 / BVH / 页划分（全部 RHI-free 纯函数）
    ${CMAKE_CURRENT_LIST_DIR}/NaniteUpload.cpp
    ${CMAKE_CURRENT_LIST_DIR}/NaniteUpload.h
    ${CMAKE_CURRENT_LIST_DIR}/NaniteTypes.h
    # T3：最小 glTF 几何读取器（不依赖 ECS；离线工具与单测共用）
    ${CMAKE_CURRENT_LIST_DIR}/NaniteGltfReader.cpp
    ${CMAKE_CURRENT_LIST_DIR}/NaniteGltfReader.h
)

# 公开包含路径：使用方按 `#include "Nanite/NaniteUpload.h"` 引用（与 Render 内部一致）
target_include_directories(HugEngineNaniteBuild
    PUBLIC ${CMAKE_SOURCE_DIR}/Engine/Render
)

# 【只加头文件路径，不链接 RHI 目标】`NaniteUpload.h` 经 `Threading/RenderThreadContext.h`
# 间接包含 `RHI/RHI.h`（帧上下文是渲染层调用 RHI 的唯一出口）。该头只用到前向声明与
# 类型，不产生对 RHI 的链接依赖 —— 这也是单测历来能"只编译 NaniteUpload.cpp、
# 不链 HugEngineRender"的原因。此处保持同样的边界：包含路径给，链接不给。
target_include_directories(HugEngineNaniteBuild
    # 【为什么是 PUBLIC】`NaniteUpload.h` 经 `Threading/RenderThreadContext.h` 间接 include
    # `RHI/RHI.h`，所以任何包含 NaniteUpload.h 的目标（单测/离线工具/Render）都要能找到它。
    # 这里只给包含路径，仍然不链接任何 RHI 目标（纪律不变）。
    PUBLIC ${CMAKE_SOURCE_DIR}/Engine/RHI
)

# 只依赖 Core（类型/日志）、meshoptimizer（簇切分算法）与 cgltf（glTF 解析）
target_link_libraries(HugEngineNaniteBuild
    PUBLIC HugEngineCore
    PRIVATE meshoptimizer
    # T3：cgltf 实现体在 Engine/External/cgltf/cgltf_impl.cpp，与 HugEngineAsset 共用同一份
    PRIVATE cgltf
)

set_target_properties(HugEngineNaniteBuild PROPERTIES
    FOLDER "Engine/Nanite"
)

# IDE 目录树：与其余模块一致，按磁盘目录分组
huge_source_group(
    ${CMAKE_CURRENT_LIST_DIR}/NaniteUpload.cpp
    ${CMAKE_CURRENT_LIST_DIR}/NaniteUpload.h
    ${CMAKE_CURRENT_LIST_DIR}/NaniteTypes.h
)
