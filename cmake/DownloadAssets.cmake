# ============================================================
# 下载 Sponza glTF 测试资源（KhronosGroup/glTF-Sample-Models）
#
# 每次 CMake configure 时检查 Content/gltf/Sponza/ 是否存在。
# 如不存在，使用 git sparse-checkout 从 GitHub 浅克隆下载
# （仅下载 2.0/Sponza/ 目录，约 50MB，非整个仓库）。
# ============================================================

set(SPONZA_CONTENT_DIR "${CMAKE_SOURCE_DIR}/Content/gltf/Sponza")
set(SPONZA_GLTF_FILE  "${SPONZA_CONTENT_DIR}/glTF/Sponza.gltf")

if(NOT EXISTS "${SPONZA_GLTF_FILE}")
    message(STATUS "Sponza glTF 资源未找到，开始从 KhronosGroup/glTF-Sample-Models 下载...")

    set(DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_sponza_download")

    # 浅克隆 + blob 过滤 + 稀疏检出，仅下载 Sponza 目录
    execute_process(
        COMMAND ${GIT_EXECUTABLE} clone
            --depth 1
            --filter=blob:none
            --sparse
            https://github.com/KhronosGroup/glTF-Sample-Models.git
            "${DOWNLOAD_DIR}"
        RESULT_VARIABLE git_clone_result
        ERROR_VARIABLE  git_clone_error
    )

    if(git_clone_result EQUAL 0)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} -C "${DOWNLOAD_DIR}" sparse-checkout set 2.0/Sponza
            RESULT_VARIABLE git_sparse_result
        )

        if(git_sparse_result EQUAL 0)
            # 复制到 Content 目录
            file(COPY "${DOWNLOAD_DIR}/2.0/Sponza/"
                 DESTINATION "${SPONZA_CONTENT_DIR}")
            message(STATUS "Sponza glTF 资源下载完成: ${SPONZA_CONTENT_DIR}")
        else()
            message(WARNING "Sponza 稀疏检出失败：将无法加载 Sponza 模型")
        endif()

        # 清理临时下载目录
        file(REMOVE_RECURSE "${DOWNLOAD_DIR}")
    else()
        message(WARNING "Sponza 下载失败（git clone 失败）: ${git_clone_error}")
    endif()
else()
    message(STATUS "Sponza glTF 资源已存在: ${SPONZA_GLTF_FILE}")
endif()
