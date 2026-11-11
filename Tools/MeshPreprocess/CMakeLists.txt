# ============================================================
#  Tools/MeshPreprocess/CMakeLists.txt
#
#  T3：离线网格预处理工具（glTF → .nanite v2）。
#
#  【为什么链 HugEngineNaniteBuild 而不是 HugEngineRender】
#  构建器、v2 读写与最小 glTF 读取器都在那个 RHI-free 静态库里；链 Render 会把
#  RHI/Vulkan/slangc 生成的头一起拖进来，既没必要也会让工具依赖 GPU 后端。
# ============================================================

add_executable(MeshPreprocess
    main.cpp
)

target_link_libraries(MeshPreprocess PRIVATE
    HugEngineNaniteBuild
    HugEngineCore
)

set_target_properties(MeshPreprocess PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    FOLDER "Tools"
)

# ============================================================
#  T6：构建期批量生成 Content/Processed/*.nanite（增量 + 失败不阻塞）
#
#  · 判据与清单都在 `process_meshes.py` 里（builderVersion/settingsHash 由工具给出）；
#  · 目标挂在 ALL 上，但"全部命中缓存"时只读一次指纹、stat 一遍目录，开销可忽略；
#  · 只处理桌面对话：移动端由 T13 的资产同步单独接入。
# ============================================================
if(WIN32)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_custom_target(ProcessMeshes ALL
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/process_meshes.py"
                    --tool "$<TARGET_FILE:MeshPreprocess>"
                    --content "${CMAKE_SOURCE_DIR}/Content"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            COMMENT "Nanite ProcessMeshes: Content/gltf → Content/Processed（增量）"
            VERBATIM
        )
        add_dependencies(ProcessMeshes MeshPreprocess)
    else()
        message(STATUS "HugEngine: 未找到 Python3 ⇒ 跳过 Nanite ProcessMeshes（运行时回退）")
    endif()
endif()