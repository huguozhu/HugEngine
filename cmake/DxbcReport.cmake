# ============================================================================
#  DxbcReport.cmake —— 汇总 D3D11 着色器的编译结果（cmake -P 脚本）
#
#  读取 OUT_DIR 下所有 *.dxbc.status，生成 OUT_DIR/dxbc_report.md：
#  通过 / 失败统计 + 每个失败着色器的首条错误，便于逐个处理（D3D11 规划 M1.3 / M5.1）。
#
#  参数（-D）：OUT_DIR
# ============================================================================

file(GLOB statuses "${OUT_DIR}/*.dxbc.status")
list(SORT statuses)

set(ok 0)
set(failed 0)
set(rows "")
foreach(st ${statuses})
    get_filename_component(fname "${st}" NAME)
    string(REGEX REPLACE "\\.dxbc\\.status$" "" name "${fname}")
    file(STRINGS "${st}" lines ENCODING UTF-8)   # 不指定编码时遇到中文字节会截断
    list(GET lines 0 head)
    if(head STREQUAL "OK")
        math(EXPR ok "${ok} + 1")
        continue()
    endif()
    math(EXPR failed "${failed} + 1")
    # 首条错误：优先取编译器的 error 行（fxc / slangc），其次取 ShaderToDxil 缩进列出的寄存器问题行
    set(first "")
    foreach(l ${lines})
        if(l MATCHES "error")
            set(first "${l}")
            break()
        endif()
    endforeach()
    if(first STREQUAL "")
        foreach(l ${lines})
            if(l MATCHES "^    ")
                string(STRIP "${l}" first)
                break()
            endif()
        endforeach()
    endif()
    string(REPLACE "|" "\\|" first "${first}")
    string(REGEX REPLACE "^FAIL " "" stage "${head}")
    string(APPEND rows "| ${name} | ${stage} | ${first} |\n")
endforeach()

math(EXPR total "${ok} + ${failed}")
file(WRITE "${OUT_DIR}/dxbc_report.md"
    "# D3D11（DXBC）着色器编译报告\n\n"
    "由 cmake/DxbcReport.cmake 在每次构建 CompileShadersDxbc 时生成。\n\n"
    "通过 ${ok} / ${total}，失败 ${failed}。\n\n"
    "| 着色器 | 失败阶段 | 首条错误 |\n|---|---|---|\n${rows}")
message(STATUS "D3D11 着色器：通过 ${ok} / ${total}，失败 ${failed}（详见 ${OUT_DIR}/dxbc_report.md）")
