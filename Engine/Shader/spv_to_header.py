#!/usr/bin/env python3
"""将 SPIR-V 二进制文件转换为 C++ uint32_t 数组头文件

用法: python spv_to_header.py <out_dir> <spv1> <spv2> ...

为每个 .spv 生成独立的 <name>.spv.h 头文件。
修改一个 shader 只重编译 include 了对应头文件的 .cpp。
"""

import sys, os, struct

out_dir = sys.argv[1]
spv_files = sys.argv[2:]

os.makedirs(out_dir, exist_ok=True)

for spv in spv_files:
    with open(spv, 'rb') as f:
        data = f.read()

    assert len(data) % 4 == 0, f"SPIR-V file size {len(data)} not multiple of 4"

    count = len(data) // 4
    words = struct.unpack(f'<{count}I', data)

    basename = os.path.basename(spv)
    name = basename.replace('.spv', '').replace('.', '_')
    var_name = f'k_{name}_spv'

    header_name = basename + '.h'
    header_path = os.path.join(out_dir, header_name)

    with open(header_path, 'w') as out:
        out.write(f'// Auto-generated from {basename} — DO NOT EDIT\n')
        out.write('#pragma once\n')
        out.write('#include <cstdint>\n')
        out.write('#include <vector>\n\n')
        out.write(f'// {basename} ({len(data)} bytes, {count} words)\n')
        out.write(f'inline const std::vector<uint32_t> {var_name} = {{\n')
        for i in range(0, count, 8):
            chunk = words[i:i+8]
            hex_vals = ', '.join(f'0x{w:08X}u' for w in chunk)
            out.write(f'    {hex_vals},\n')
        out.write('};\n')
