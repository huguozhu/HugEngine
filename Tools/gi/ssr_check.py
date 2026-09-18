"""ssr_check.py -- assertions for Tools/gi/ssr_check.ps1 (docs 9.2-W / task 25).

Reads the dumps written by the .ps1 and applies three assertions:

  1. the Hi-Z path produces hits: validity fraction (alpha > 0) in the SSR output >= 5%
     (before task 25 it was 0% -- RGB=0 and alpha=-1 on every pixel);
  2. Hi-Z finds at least half as many hits as the linear march (same order of magnitude);
  3. putting SSR into a specular stack that already has IBL changes the HDR reading
     (before task 25 the two were pixel-identical, so "SSR enabled" meant nothing).

Exit code 0 = all assertions pass.
"""
import glob
import os
import sys

import numpy as np

MIN_HIZ_VALID = 0.05      # Hi-Z validity fraction floor (was exactly 0 before the fix)
MIN_HIZ_VS_LIN = 0.5      # Hi-Z must find at least half as many hits as linear march
MIN_HDR_CHANGE = 0.001    # relative change of the HDR mean when SSR joins the stack


def load(path):
    a = np.fromfile(path, dtype=np.float16).astype(np.float32)
    if a.size % 4 != 0:
        raise SystemExit("unexpected dump size: %s" % path)
    return a.reshape(-1, 4)


def ssr_target(directory, tag):
    """SSR's own output. Dump names providers by registration order: 4 = SSR."""
    path = os.path.join(directory, "gi_%s_prov4_spec_raw.f16" % tag)
    if os.path.exists(path):
        return path
    cands = sorted(glob.glob(os.path.join(directory, "gi_%s_prov*_spec_raw.f16" % tag)))
    if not cands:
        raise SystemExit("no spec_raw dump for %s" % tag)
    return max(cands, key=os.path.getsize)   # fall back to the largest one


def hdr_mean(directory, tag):
    path = os.path.join(directory, "gi_%s_hdr.f16" % tag)
    a = load(path)
    return float((0.2126 * a[:, 0] + 0.7152 * a[:, 1] + 0.0722 * a[:, 2]).mean())


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else "."
    failures = []
    # 标签 -> dump 前缀（.ps1 里写成 ssr_<label>，与脚本里的 name 保持一致）
    TAGS = {"hiz": "ssr_hiz", "lin": "ssr_lin", "ibl": "ssr_ibl", "ibl_ssr": "ssr_ibl_ssr"}

    def verdict(name, ok, detail):
        print("  [%s] %s -- %s" % ("PASS" if ok else "FAIL", name, detail))
        if not ok:
            failures.append(name)

    print("=== SSR output validity (alpha > 0) ===")
    valid = {}
    for label in ("hiz", "lin"):
        path = ssr_target(directory, TAGS[label])
        a = load(path)
        valid[label] = float((a[:, 3] > 0.0).mean())
        nz = float((np.abs(a[:, :3]).sum(axis=1) > 1e-6).mean())
        print("  %-4s %-46s valid=%.2f%%  rgb!=0=%.2f%%"
              % (label, os.path.basename(path), valid[label] * 100.0, nz * 100.0))

    print("")
    print("=== verdicts ===")
    verdict("Hi-Z path produces hits", valid.get("hiz", 0.0) >= MIN_HIZ_VALID,
            "valid=%.2f%% (>= %.0f%%; was 0%% before task 25)"
            % (valid.get("hiz", 0.0) * 100.0, MIN_HIZ_VALID * 100.0))
    lin = valid.get("lin", 0.0)
    verdict("Hi-Z is in the same ballpark as the linear march",
            lin > 0.0 and valid.get("hiz", 0.0) >= MIN_HIZ_VS_LIN * lin,
            "Hi-Z %.2f%% vs linear %.2f%% (ratio %.2f, threshold >= %.1f)"
            % (valid.get("hiz", 0.0) * 100.0, lin * 100.0,
               (valid.get("hiz", 0.0) / lin) if lin > 0.0 else 0.0, MIN_HIZ_VS_LIN))

    try:
        h_ibl = hdr_mean(directory, TAGS["ibl"])
        h_both = hdr_mean(directory, TAGS["ibl_ssr"])
        rel = abs(h_both - h_ibl) / max(abs(h_ibl), 1e-9)
        verdict("SSR changes the picture", rel >= MIN_HDR_CHANGE,
                "HDR {IBL}=%.7f vs {IBL,SSR}=%.7f (relative %.3f%%, threshold >= %.1f%%)"
                % (h_ibl, h_both, rel * 100.0, MIN_HDR_CHANGE * 100.0))
    except SystemExit as e:
        verdict("SSR changes the picture", False, str(e))

    if failures:
        print("")
        print("!! %d verdict(s) failed: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("")
    print("all verdicts passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
