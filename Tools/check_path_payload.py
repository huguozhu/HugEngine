#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_path_payload.py — 校验路径追踪载荷的 C++ / Slang 两端布局一致（PT 任务 1）

背景
----
`PathPayload` 在两处各写了一份：
  · Slang：Engine/Shader/Shaders/PT_Common.slang
  · C++ ：Engine/Render/RT/PathPayload.h（另有 static_assert + offsetof 断言）

C++ 侧能用 `static_assert(sizeof(...) == 96)` 与 `offsetof` 钉住自己的布局，
但 Slang 编译器不支持 `sizeof` 形式的静态断言（实测 `static_assert(sizeof(X) == N)`
报 E20001 `unexpected token`）。所以两端的一致性改由本脚本校验：
解析两份源码里的字段声明顺序与类型，按 16 字节对齐（float4）推算偏移，
再与 C++ 侧 `static_assert(offsetof(...) == N)` 里写的真值逐一比对。

判据（任一不满足即 FAIL，退出码 1）
-----------------------------------
  1. 字段名序列完全一致（顺序敏感）
  2. 每个字段类型在两端的“标量宽度”一致（float4 ↔ float4）
  3. 按声明顺序推算的偏移 == C++ 侧 offsetof 断言里写的值
  4. 结构总大小 == C++ 侧 sizeof 断言的字节数

用法
----
    python Tools/check_path_payload.py            # 在仓库根目录执行
退出码 0 = 一致；1 = 不一致（打印差异）；2 = 解析失败。
"""

import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SLANG_PATH = os.path.join(ROOT, "Engine", "Shader", "Shaders", "PT_Common.slang")
CPP_PATH = os.path.join(ROOT, "Engine", "Render", "RT", "PathPayload.h")

STRUCT_NAME = "PathPayload"

# Slang / C++ 都会用到的标量类型宽度（用于按对齐规则推算偏移）
SCALAR_SIZE = {
    "float": 4, "int": 4, "uint": 4, "bool": 4,
    "float2": 8, "int2": 8, "uint2": 8,
    "float3": 12, "int3": 12, "uint3": 12,
    "float4": 16, "int4": 16, "uint4": 16,
}


def read_text(path):
    with io.open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def extract_struct_body(text, name):
    """提取 `struct <name> { ... };` 的花括号内文本（第一处匹配）。"""
    m = re.search(r"struct\s+" + re.escape(name) + r"\s*\{(.*?)\n\};", text, re.S)
    if not m:
        raise ValueError("未找到 struct %s 的定义" % name)
    return m.group(1)


def parse_fields(body):
    """解析字段声明：`<type> <name> [= init];` 形式，忽略注释与宏。返回 [(type, name)]。"""
    fields = []
    for raw in body.splitlines():
        line = raw.strip()
        if not line or line.startswith("//"):
            continue
        # 去掉行尾注释
        line = re.sub(r"//.*$", "", line).strip()
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(=|;)", line)
        if not m:
            continue
        ftype, fname = m.group(1), m.group(2)
        if ftype not in SCALAR_SIZE:
            raise ValueError("未知字段类型：%s %s" % (ftype, fname))
        fields.append((ftype, fname))
    return fields


def compute_offsets(fields):
    """按“声明顺序 + 每个字段自然对齐”推算偏移（两端都是紧凑的 float4 序列）。"""
    offsets = {}
    pos = 0
    max_align = 1
    for ftype, fname in fields:
        size = SCALAR_SIZE[ftype]
        align = min(size, 16) if size >= 4 else size
        if pos % align:
            pos += align - (pos % align)
        offsets[fname] = pos
        pos += size
        max_align = max(max_align, align)
    if pos % max_align:
        pos += max_align - (pos % max_align)
    return offsets, pos


def parse_cpp_assertions(text):
    """提取 C++ 侧断言：sizeof == N、offsetof(PathPayload, f) == N。"""
    size = None
    m = re.search(r"static_assert\s*\(\s*sizeof\s*\(\s*" + STRUCT_NAME + r"\s*\)\s*==\s*(\d+)", text)
    if m:
        size = int(m.group(1))
    offs = {}
    for m in re.finditer(
            r"static_assert\s*\(\s*offsetof\s*\(\s*" + STRUCT_NAME + r"\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*==\s*(\d+)",
            text):
        offs[m.group(1)] = int(m.group(2))
    return size, offs


def main():
    problems = []

    try:
        slang_fields = parse_fields(extract_struct_body(read_text(SLANG_PATH), STRUCT_NAME))
        cpp_fields = parse_fields(extract_struct_body(read_text(CPP_PATH), STRUCT_NAME))
    except (ValueError, IOError) as e:
        print("解析失败：%s" % e)
        return 2

    cpp_size, cpp_offsets = parse_cpp_assertions(read_text(CPP_PATH))

    # 判据 1/2：字段名与类型序列一致
    if [n for _, n in slang_fields] != [n for _, n in cpp_fields]:
        problems.append("字段名/顺序不一致：\n  Slang: %s\n  C++  : %s"
                        % ([n for _, n in slang_fields], [n for _, n in cpp_fields]))
    else:
        for (st, sn), (ct, cn) in zip(slang_fields, cpp_fields):
            if SCALAR_SIZE[st] != SCALAR_SIZE[ct]:
                problems.append("字段 %s 类型宽度不一致：Slang %s(%d) vs C++ %s(%d)"
                                % (sn, st, SCALAR_SIZE[st], ct, SCALAR_SIZE[ct]))

    # 判据 3/4：偏移与总大小
    computed, total = compute_offsets(slang_fields)
    if cpp_size is None:
        problems.append("C++ 侧缺少 static_assert(sizeof(%s) == N)" % STRUCT_NAME)
    elif cpp_size != total:
        problems.append("结构大小不一致：由字段推算 %d，C++ 断言 %d" % (total, cpp_size))

    if not cpp_offsets:
        problems.append("C++ 侧缺少 offsetof 断言")
    for fname, off in computed.items():
        if fname not in cpp_offsets:
            problems.append("字段 %s 缺少 C++ 侧 offsetof 断言" % fname)
        elif cpp_offsets[fname] != off:
            problems.append("字段 %s 偏移不一致：由字段推算 %d，C++ 断言 %d"
                            % (fname, off, cpp_offsets[fname]))
    for fname in cpp_offsets:
        if fname not in computed:
            problems.append("C++ 侧断言了 Slang 中不存在的字段：%s" % fname)

    print("%s：Slang %d 字段 / C++ %d 字段，推算大小 %d B（C++ 断言 %s）"
          % (STRUCT_NAME, len(slang_fields), len(cpp_fields), total,
             cpp_size if cpp_size is not None else "缺失"))
    for ftype, fname in slang_fields:
        print("    %-16s %-16s offset=%d" % (ftype, fname, computed[fname]))

    if problems:
        print("\n不一致：")
        for p in problems:
            print("  - %s" % p)
        return 1
    print("\n两端布局一致（字段名/顺序/类型/偏移/总大小）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
