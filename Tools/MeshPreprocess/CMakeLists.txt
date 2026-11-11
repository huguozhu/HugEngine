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
