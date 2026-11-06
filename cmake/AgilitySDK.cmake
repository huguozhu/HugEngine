# ============================================================
# D3D12 Agility SDK（Microsoft.Direct3D.D3D12 NuGet 包）
#
# 作用：程序自带一份 D3D12 运行时（D3D12Core.dll）与调试层（d3d12SDKLayers.dll），
#       不依赖用户 Windows 版本里的系统运行时 ⇒ 老系统上也能用到新特性
#       （SM 6.8、增强屏障等），调试层也随之更新。
#
# 做法：
#   1. configure 时从 nuget.org 下载指定版本的 nupkg（校验 SHA256），解压到构建目录；
#      已存在则跳过。下载失败只告警，D3D12 后端退回系统运行时。
#   2. huge_enable_d3d12_agility(<exe 目标>)：给可执行文件加上导出符号
#      D3D12SDKVersion / D3D12SDKPath（必须由 exe 导出，放在静态库里不生效），
#      并把两个 DLL 复制到 exe 同目录的 D3D12/ 子目录（独立的复制目标 + 依赖：样例目标定义在
#      各自的子目录里，跨目录不能给它们挂 POST_BUILD 命令）。
# ============================================================

option(HUGENGINE_D3D12_AGILITY "D3D12 后端使用 Agility SDK 运行时" ON)

# 包版本与包文件的 SHA256（升级时两者一起改）
set(HE_AGILITY_PACKAGE_VERSION "1.620.1")
set(HE_AGILITY_PACKAGE_SHA256  "a8a494c3e5f687d9be0a465ec8248da70781ab689d888db8806b600422722be6")

set(HE_AGILITY_READY OFF)
if(WIN32 AND HUGENGINE_D3D12_AGILITY)
    set(_agility_dir  "${CMAKE_BINARY_DIR}/_deps/agility_sdk/${HE_AGILITY_PACKAGE_VERSION}")
    set(_agility_pkg  "${CMAKE_BINARY_DIR}/_deps/agility_sdk/microsoft.direct3d.d3d12.${HE_AGILITY_PACKAGE_VERSION}.nupkg")
    set(_agility_hdr  "${_agility_dir}/build/native/include/d3d12.h")

    if(NOT EXISTS "${_agility_hdr}")
        message(STATUS "Agility SDK ${HE_AGILITY_PACKAGE_VERSION} 未找到，开始从 nuget.org 下载...")
        file(DOWNLOAD
            "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/${HE_AGILITY_PACKAGE_VERSION}/microsoft.direct3d.d3d12.${HE_AGILITY_PACKAGE_VERSION}.nupkg"
            "${_agility_pkg}"
            EXPECTED_HASH SHA256=${HE_AGILITY_PACKAGE_SHA256}
            STATUS _agility_status
            TLS_VERIFY ON)
        list(GET _agility_status 0 _agility_code)
        if(_agility_code EQUAL 0)
            # nupkg 就是 zip 包
            file(ARCHIVE_EXTRACT INPUT "${_agility_pkg}" DESTINATION "${_agility_dir}")
            file(REMOVE "${_agility_pkg}")
        else()
            list(GET _agility_status 1 _agility_msg)
            message(WARNING "Agility SDK 下载失败（${_agility_msg}），D3D12 后端将使用系统运行时")
            file(REMOVE "${_agility_pkg}")
        endif()
    endif()

    if(EXISTS "${_agility_hdr}")
        # SDK 版本号从包内 d3d12.h 读取，保证与 D3D12Core.dll 一致（导出的 D3D12SDKVersion 必须等于它）
        file(STRINGS "${_agility_hdr}" _agility_line REGEX "#define[ \t]+D3D12_SDK_VERSION[ \t]+\\([ \t]*[0-9]+[ \t]*\\)")
        # 只取括号里的数字（宏名 D3D12 本身也含数字，不能直接匹配第一个数字）
        string(REGEX MATCH "\\([ \t]*([0-9]+)" _agility_match "${_agility_line}")
        set(HE_AGILITY_SDK_VERSION "${CMAKE_MATCH_1}")
        set(HE_AGILITY_BIN_DIR "${_agility_dir}/build/native/bin/x64")
        if(HE_AGILITY_SDK_VERSION AND EXISTS "${HE_AGILITY_BIN_DIR}/D3D12Core.dll")
            set(HE_AGILITY_READY ON)
            message(STATUS "Agility SDK: ${HE_AGILITY_PACKAGE_VERSION}（D3D12_SDK_VERSION=${HE_AGILITY_SDK_VERSION}）")
        else()
            message(WARNING "Agility SDK 包内容不完整，D3D12 后端将使用系统运行时")
        endif()
    endif()
endif()

# 给可执行文件启用 Agility SDK：导出版本号 / 路径符号，构建后复制运行时 DLL
function(huge_enable_d3d12_agility target)
    if(NOT HE_AGILITY_READY OR NOT TARGET ${target})
        return()
    endif()
    target_sources(${target} PRIVATE "${CMAKE_SOURCE_DIR}/Engine/RHI/D3D12/D3D12AgilityExports.cpp")
    target_compile_definitions(${target} PRIVATE HE_D3D12_AGILITY_SDK_VERSION=${HE_AGILITY_SDK_VERSION})
    add_custom_target(${target}_AgilityRuntime
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/D3D12"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${HE_AGILITY_BIN_DIR}/D3D12Core.dll"
                "${HE_AGILITY_BIN_DIR}/d3d12SDKLayers.dll"
                "$<TARGET_FILE_DIR:${target}>/D3D12/"
        COMMENT "Copy Agility SDK runtime for ${target}")
    set_target_properties(${target}_AgilityRuntime PROPERTIES FOLDER "Samples/Agility")
    add_dependencies(${target} ${target}_AgilityRuntime)
endfunction()
