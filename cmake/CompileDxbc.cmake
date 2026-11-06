# ============================================================================
#  CompileDxbc.cmake —— 单个着色器的 D3D11 字节码生成（cmake -P 脚本）
#
#  链路：slangc -target hlsl  →  ShaderToDxil --dxbc（补丁 + D3DCompile）  →  X.dxbc.h
#
#  【失败不阻塞构建】D3D11（SM 5.0）有很多着色器暂时编不过（bindless 数组、fxc 内部错误等，
#  逐步在 M3 / M5 解决）。任何一步失败时：
#    · 仍然写出 X.dxbc.h，内容是 1 字节的占位数组（不以 "DXBC" 开头），
#      运行时 D3D11 后端据此判定「该着色器在 D3D11 下不可用」，引用它的 pass 照常编译链接；
#    · 把失败原因写进 X.dxbc.status（第一行 OK / FAIL，后面是日志），供汇总报告使用。
#  脚本自身总是成功返回。
#
#  参数（-D）：SLANGC SHADER_TO_DXIL SRC NAME ENTRY SLANG_STAGE STAGE SYM SHADER_DIR OUT_DIR
# ============================================================================

set(HLSL   "${OUT_DIR}/${NAME}.dxbc.hlsl")
set(HEADER "${OUT_DIR}/${NAME}.dxbc.h")
set(STATUS "${OUT_DIR}/${NAME}.dxbc.status")
file(MAKE_DIRECTORY "${OUT_DIR}")

# 写占位头文件 + 失败状态（reason 为失败阶段，log 为该阶段的输出）
function(write_failure reason log)
    file(WRITE "${HEADER}"
        "// 本文件由 cmake/CompileDxbc.cmake 自动生成，请勿手改。\n"
        "// ${NAME} 在 D3D11 下编译失败（${reason}），这里只是占位：内容不是 DXBC，\n"
        "// D3D11 后端据此判定该着色器不可用。失败原因见同目录的 ${NAME}.dxbc.status。\n"
        "#pragma once\n"
        "static const unsigned char ${SYM}[] = { 0 };\n"
        "static const unsigned int ${SYM}_size = 0u;\n")
    file(WRITE "${STATUS}" "FAIL ${reason}\n${log}")
endfunction()

# 第一段：Slang → HLSL（D3D11：HE_SHADER_HLSL + HE_SHADER_D3D11）
execute_process(
    COMMAND "${SLANGC}" "${SRC}" -target hlsl -entry ${ENTRY} -stage ${SLANG_STAGE}
            -DHE_SHADER_HLSL -DHE_SHADER_D3D11 -I "${SHADER_DIR}" -o "${HLSL}" -Wno-39001
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE  out)
if(NOT rc EQUAL 0)
    write_failure("slangc" "${out}")
    message(STATUS "DXBC 失败（slangc）：${NAME}")
    return()
endif()

# 第二段：ShaderToDxil --dxbc → X.dxbc.h
execute_process(
    COMMAND "${SHADER_TO_DXIL}" --dxbc "${HLSL}" ${ENTRY} ${STAGE} "${HEADER}" ${SYM}
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE  out)
if(NOT rc EQUAL 0)
    write_failure("ShaderToDxil" "${out}")
    message(STATUS "DXBC 失败（ShaderToDxil）：${NAME}")
    return()
endif()

file(WRITE "${STATUS}" "OK\n${out}")
