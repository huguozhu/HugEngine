#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""analyze_pt.py — PT 参考图落盘的离线读数（PT 任务 5）

两种用法
--------
1) 可复现性检查（判据：连续 N 次运行读数离散 < 阈值）
       python Tools/pt/analyze_pt.py repro_run1 repro_run2 repro_run3
   逐 tag 打印统计量，并给出所有 tag 之间的**最大相对偏差**；相对偏差
   超过阈值（默认 1e-4）即判为不可复现，退出码 1。

2) 两版对照（例如「任务 2 的 exe」vs「任务 3 的 exe」）
       python Tools/pt/analyze_pt.py --diff before after
   逐像素比较，打印最大绝对差 / RMSE / 超过容差的像素比例；任何像素
   超过容差（默认 0）即退出码 1（用于"逐像素一致"这类判据）。

数据格式
--------
`pt_<tag>_<target>.f16`：原始像素、无文件头、行紧密排布（RGBA16F 或 R32F，
格式见 `pt_<tag>_meta.txt`）。由 Tools/pt/dump_pt.ps1 落盘。
"""

import argparse
import io
import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "build", "verify")


def read_meta(tag):
    """读 pt_<tag>_meta.txt → {target: (w, h, fmt, dtype)}。"""
    path = os.path.join(OUT, "pt_%s_meta.txt" % tag)
    meta = {}
    if not os.path.isfile(path):
        return meta
    with io.open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) != 4:
                continue
            name, w, h, fmt = parts[0], int(parts[1]), int(parts[2]), parts[3]
            meta[name] = (w, h, fmt, np.float16 if fmt == "RGBA16F" else np.float32)
    return meta


def load(tag, target):
    """读一个落盘目标 → (array, w, h, channels)。"""
    meta = read_meta(tag)
    if target not in meta:
        raise SystemExit("缺少目标 %s（tag=%s）；现有: %s" % (target, tag, sorted(meta)))
    w, h, fmt, dtype = meta[target]
    path = os.path.join(OUT, "pt_%s_%s.f16" % (tag, target))
    if not os.path.isfile(path):
        raise SystemExit("缺少落盘文件: %s" % path)
    ch = 4 if fmt == "RGBA16F" else 1
    data = np.fromfile(path, dtype=dtype)
    if data.size != w * h * ch:
        raise SystemExit("尺寸不符: %s 期望 %d 实际 %d" % (path, w * h * ch, data.size))
    return data.reshape(h, w, ch).astype(np.float32), w, h, ch


def stats(arr):
    """线性 HDR 的读数（只在 hdr 目标上有物理含义，其它目标仅作定位参考）。"""
    lum = arr[..., :3].mean(axis=2) if arr.shape[2] >= 3 else arr[..., 0]
    return {
        "mean": float(lum.mean()),
        "p50": float(np.percentile(lum, 50)),
        "p95": float(np.percentile(lum, 95)),
        "max": float(lum.max()),
        "nonzero_ratio": float((lum > 1e-6).mean()),
    }


def cmd_repro(tags, target, tol):
    rows = []
    for tag in tags:
        arr, w, h, _ = load(tag, target)
        s = stats(arr)
        rows.append((tag, s, arr))
        print("%-16s %dx%d  mean=%.6f p50=%.6f p95=%.6f max=%.6f nonzero=%.4f"
              % (tag, w, h, s["mean"], s["p50"], s["p95"], s["max"], s["nonzero_ratio"]))

    worst = 0.0
    worst_key = ""
    base_tag, base, base_arr = rows[0]
    for tag, s, arr in rows[1:]:
        for key in ("mean", "p50", "p95", "max", "nonzero_ratio"):
            denom = max(abs(base[key]), 1e-12)
            rel = abs(s[key] - base[key]) / denom
            if rel > worst:
                worst, worst_key = rel, "%s.%s(%s vs %s)" % (key, "rel", tag, base_tag)
        # 逐像素最大差（同一场景同帧应完全确定）
        if arr.shape == base_arr.shape:
            pix = float(np.abs(arr - base_arr).max())
            print("    %s vs %s: 逐像素最大绝对差 = %.3e" % (tag, base_tag, pix))

    print("\n最大相对偏差: %.3e  (%s)   阈值: %.1e" % (worst, worst_key, tol))
    if worst > tol:
        print("判定：不可复现（超过阈值）")
        return 1
    print("判定：可复现（在阈值内）")
    return 0


def cmd_diff(a, b, target, tol, rel_tol, strict):
    arr_a, wa, ha, _ = load(a, target)
    arr_b, wb, hb, _ = load(b, target)
    if arr_a.shape != arr_b.shape:
        raise SystemExit("两版尺寸不同: %s %s vs %s %s" % (a, arr_a.shape, b, arr_b.shape))

    d = np.abs(arr_a - arr_b)
    max_abs = float(d.max())
    rmse = float(np.sqrt((d ** 2).mean()))
    denom = np.maximum(np.maximum(np.abs(arr_a), np.abs(arr_b)), 1e-12)
    rel = d / denom
    n_diff = int((d > 0).sum())

    # 判定：|d| <= tol + rel_tol * max(|a|,|b|)。strict 模式要求逐位相同。
    allowed = tol + rel_tol * np.maximum(np.abs(arr_a), np.abs(arr_b))
    over_mask = d > allowed
    over = int(over_mask.sum())

    print("目标 %s  %dx%d  元素数 %d" % (target, wa, ha, d.size))
    print("  非零差异元素        = %d (%.6f%%)" % (n_diff, 100.0 * n_diff / d.size))
    print("  最大绝对差          = %.6e" % max_abs)
    print("  最大相对差          = %.6e" % (float(rel[d > 0].max()) if n_diff else 0.0))
    print("  RMSE                = %.6e" % rmse)
    print("  超过容差(%.1e + %.1e·|值|) 的元素 = %d" % (tol, rel_tol, over))

    if strict:
        ok = (max_abs == 0.0)
        print("判定（严格逐位）：%s" % ("一致" if ok else "存在差异"))
        return 0 if ok else 1
    print("判定：%s" % ("一致（在容差内）" if over == 0 else "存在差异（超过容差）"))
    return 0 if over == 0 else 1


def main():
    ap = argparse.ArgumentParser(description="PT 落盘读数 / 两版对照")
    ap.add_argument("tags", nargs="*", help="可复现性检查用的 tag 列表（>=2）")
    ap.add_argument("--diff", nargs=2, metavar=("TAG_A", "TAG_B"), help="两版逐像素对照")
    ap.add_argument("--target", default="hdr", help="落盘目标名（默认 hdr）")
    ap.add_argument("--tol", type=float, default=1e-4, help="绝对容差（默认 1e-4）")
    ap.add_argument("--rel-tol", type=float, default=1e-3,
                    help="相对容差（默认 1e-3；函数调用/内联差异会带来 1e-4~1e-3 量级的末位差）")
    ap.add_argument("--strict", action="store_true", help="要求逐位相同（差分必须为 0）")
    args = ap.parse_args()

    if args.diff:
        return cmd_diff(args.diff[0], args.diff[1], args.target, args.tol, args.rel_tol, args.strict)
    if len(args.tags) < 2:
        ap.error("请给出 >=2 个 tag（可复现性检查）或使用 --diff")
    return cmd_repro(args.tags, args.target, args.tol)


if __name__ == "__main__":
    sys.exit(main())
