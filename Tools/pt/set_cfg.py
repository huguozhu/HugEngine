#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""set_cfg.py — 改写示例 cfg 副本里的键值（PT 对照实验模板用）

用法：
    python Tools/pt/set_cfg.py <cfg 路径> key=value [key=value ...]

行为：键存在则就地替换整行；不存在则追加到文件末尾。
为什么需要它：示例的 ImGui 参数由 cfg 恢复，做「SPP 维收敛」这类实验时
必须在**私有副本**上改参数（禁止改基准 cfg），改完再交给 dump 脚本运行。
"""

import io
import sys


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    path = sys.argv[1]
    pairs = []
    for arg in sys.argv[2:]:
        if "=" not in arg:
            print("参数格式应为 key=value: %s" % arg)
            return 2
        k, v = arg.split("=", 1)
        pairs.append((k.strip(), v.strip()))

    try:
        with io.open(path, "r", encoding="utf-8", errors="replace", newline="") as f:
            lines = f.read().split("\n")
    except IOError:
        lines = []

    have_eol = len(lines) > 1 or (len(lines) == 1 and lines[0] != "")
    for key, val in pairs:
        replaced = False
        for i, line in enumerate(lines):
            stripped = line.rstrip("\r")
            if stripped.startswith(key + "="):
                lines[i] = "%s=%s" % (key, val)
                replaced = True
                break
        if not replaced:
            # 去掉尾部的空串（split 的产物），追加新键
            while lines and lines[-1] == "":
                lines.pop()
            lines.append("%s=%s" % (key, val))
            lines.append("")
        print("set %s=%s" % (key, val))

    with io.open(path, "w", encoding="utf-8", newline="") as f:
        f.write("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
