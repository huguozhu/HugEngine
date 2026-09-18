"""rsm_indirect_check.py -- assertions for Tools/gi/rsm_indirect_check.ps1 (task 16 / 9.2-AA).

Reads the two pass-timing logs written by the .ps1 (chk_rsmind_none.log / chk_rsmind_rsm.log)
plus the HDR dumps and the RSM-chain dumps, and applies these assertions:

  1. the half-resolution `RSM_Indirect` pass ran and its GPU time is non-zero;
  2. the `RSM` raster pass is present in the RSM case (a sanity check that case B really had
     RSM enabled rather than silently skipped).
  3. TASK 30 -- the RSM chain must actually PRODUCE DATA at every level. This is the
     assertion that would have failed for the whole lifetime of defect 9.2-AA, where the
     pass ran, the cost was paid, and all three RSM attachments held nothing but the clear
     value (a caller-side out-of-bounds depth clear value rejected every fragment; and the
     VPL albedo was read out of an object buffer with a different index space). The dump
     targets `rsm_pos` / `rsm_nrm` / `rsm_rad` / `rsm_indirect` exist for exactly this.
  4. the source must reach the HDR at all (S_rsm > 0 with a low floor). S_rsm is small by
     construction (a 16-tap gather of a 2.5D shadow map integrates only a small disk of the
     hemisphere and most VPL pairs are co-planar, so their cosine terms vanish), so the
     floor is deliberately low: what must not happen is "exactly zero".

NOTE ON THE TIMING VERDICT THAT IS *NOT* HERE: task 16's claim ("the 16-tap VPL sum no longer
runs per full-resolution pixel inside Lighting") was measured with `HE_PASS_TIMING` before and
after the change (Lighting 0.882 -> 0.513 ms, with the untouched `GB_Clear` pass as a control;
see docs 10.2 task 16). It is deliberately NOT a pass/fail criterion here: per-pass GPU times
on this machine drift enough between runs to make any threshold unreliable -- the same binary
gave `Lighting(rsm)` 0.774 and 1.105 ms in two consecutive check runs, and `GB_Clear`
1.44 / 1.79 / 2.08 ms across sessions. What this check guards instead is the *structure*
(the dedicated pass exists and runs), because that is what would break if someone merged the
sum back into Lighting.

Exit code 0 = all assertions pass.
"""
import os
import re
import sys

import numpy as np

W, H = 1920, 1080
PASS_MIN_MS = 0.02             # RSM_Indirect must be a measurable, non-zero pass
RAD_NONZERO_MIN = 0.20         # the VPL radiance map must actually cover geometry
INDIRECT_NONZERO_MIN = 0.20    # the half-resolution result must actually produce something
RAD_MEAN_MIN = 0.005           # ...and be a real radiance, not numerical dust
S_RSM_MIN = 1e-6               # the source must reach the HDR (see note in the docstring)


def parse_pass_times(log_path):
    """Return {pass name: gpu ms} from the last '[Pass ' line of the log."""
    if not os.path.exists(log_path):
        raise SystemExit("missing log: %s" % log_path)
    line = None
    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        for l in f:
            if "[Pass " in l:
                line = l
    if line is None:
        raise SystemExit("no pass timing line in %s (is HE_PASS_TIMING=1 set?)" % log_path)
    out = {}
    for name, ms in re.findall(r"([A-Za-z_][A-Za-z0-9_]*)=(-?\d+\.\d+)ms", line):
        out[name] = float(ms)
    return out


def load_px(path, w=None, h=None):
    a = np.fromfile(path, dtype=np.float16).astype(np.float32)
    if w is None:
        # RSM textures are square; the half-resolution result is W/2 x H/2
        px = a.size // 4
        side = int(px ** 0.5)
        w, h = (side, side) if side * side == px else (W // 2, H // 2)
    return a.reshape(h, w, 4)


def load_lum(directory, tag):
    return lum(load_px(os.path.join(directory, "gi_%s_hdr.f16" % tag), W, H))


def lum(a):
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else "."
    none_t = parse_pass_times(os.path.join(directory, "chk_rsmind_none.log"))
    rsm_t = parse_pass_times(os.path.join(directory, "chk_rsmind_rsm.log"))

    failures = []

    def verdict(name, ok, detail):
        print("  [%s] %s -- %s" % ("PASS" if ok else "FAIL", name, detail))
        if not ok:
            failures.append(name)

    print("=== pass GPU times (ms) ===")
    print("  empty diffuse stack : Lighting=%.3f" % none_t.get("Lighting", -1.0))
    print("  diffuse={RSM}       : Lighting=%.3f RSM=%.3f RSM_Indirect=%.3f"
          % (rsm_t.get("Lighting", -1.0), rsm_t.get("RSM", -1.0),
             rsm_t.get("RSM_Indirect", -1.0)))

    print("")
    print("=== verdicts ===")
    ri = rsm_t.get("RSM_Indirect", 0.0)
    verdict("half-resolution RSM_Indirect pass runs", ri >= PASS_MIN_MS,
            "RSM_Indirect=%.3f ms (>= %.2f)" % (ri, PASS_MIN_MS))
    verdict("RSM raster pass present in the RSM case", rsm_t.get("RSM", 0.0) > 0.0,
            "RSM=%.3f ms" % rsm_t.get("RSM", 0.0))

    # ---- 任务 30：链路逐级必须真有产出 ----------------------------------------
    try:
        rad = load_px(os.path.join(directory, "gi_rsmind_rsm_rsm_rad.f16"))
        pos = load_px(os.path.join(directory, "gi_rsmind_rsm_rsm_pos.f16"))
        ind = load_px(os.path.join(directory, "gi_rsmind_rsm_rsm_indirect.f16"))
    except OSError as e:
        print("  !! missing RSM chain dump: %s" % e)
        failures.append("rsm chain dumps present")
        rad = pos = ind = None

    if rad is not None:
        rad_lum = lum(rad)
        pos_cov = (np.abs(pos[..., 0]) + np.abs(pos[..., 1]) + np.abs(pos[..., 2])) > 0
        ind_lum = lum(ind)
        print("")
        print("  dumps: rsm_pos covered=%.2f%%  rsm_rad nonzero=%.2f%% mean=%.4f  "
              "rsm_indirect nonzero=%.2f%% mean=%.3e max=%.3f"
              % (100.0 * pos_cov.mean(), 100.0 * (rad_lum > 0).mean(), rad_lum.mean(),
                 100.0 * (ind_lum > 0).mean(), ind_lum.mean(), ind_lum.max()))

        # 位置图必须有几何（深度清除值坏掉时这里是纯 0）
        verdict("RSM position map holds geometry", pos_cov.mean() > 0.20,
                "covered=%.2f%%" % (100.0 * pos_cov.mean()))
        # 法线图必须与位置图有相同的覆盖（同一 pass 的三个附件一起写）。
        # 判据用"编码法线非零"：未覆盖的 texel 是清除值 (0,0,0,1)，而覆盖的 texel 编码后
        # 三分量至少有一个 >= 0.21（单位法线的分量不可能三个都接近 -1）。
        # 【不要用 |c-0.5|】：清除值 0 与 0.5 的差也是 0.5，会把"没覆盖"判成"覆盖"（踩过一次）。
        nrm = load_px(os.path.join(directory, "gi_rsmind_rsm_rsm_nrm.f16"))
        nrm_cov = nrm[..., :3].sum(axis=2) > 0.05
        verdict("RSM normal map covers the same texels",
                abs(nrm_cov.mean() - pos_cov.mean()) < 0.02,
                "nrm=%.2f%% vs pos=%.2f%%" % (100.0 * nrm_cov.mean(), 100.0 * pos_cov.mean()))
        # VPL 辐射度：既要有覆盖，也要是真量级（albedo 读错槽时这里会掉到 3% 且均值极小）
        verdict("VPL radiance map produces real radiance",
                (rad_lum > 0).mean() > RAD_NONZERO_MIN and rad_lum.mean() > RAD_MEAN_MIN,
                "nonzero=%.2f%% (>= %.0f%%) mean=%.4f (>= %.3f)"
                % (100.0 * (rad_lum > 0).mean(), 100.0 * RAD_NONZERO_MIN,
                   rad_lum.mean(), RAD_MEAN_MIN))
        # 半分辨率结果
        verdict("half-resolution RSM indirect produces output",
                (ind_lum > 0).mean() > INDIRECT_NONZERO_MIN,
                "nonzero=%.2f%% (>= %.0f%%)" % (100.0 * (ind_lum > 0).mean(),
                                                100.0 * INDIRECT_NONZERO_MIN))

    # ---- 源必须真的到达 HDR（S_rsm 不再恒为 0）--------------------------------
    try:
        s = load_lum(directory, "rsmind_rsm") - load_lum(directory, "rsmind_none")
        # 与"预期贡献 = E/pi * albedo"的形状一致性：只用它做 print，不做判据
        corr = None
        try:
            alb = load_px(os.path.join(directory, "gi_rsmind_rsm_albedo.f16"), W, H)
            exp = np.repeat(np.repeat(lum(ind) * lum(alb[::2, ::2, :]), 2, axis=0), 2, axis=1)
            corr = float(np.corrcoef(s.ravel(), exp.ravel())[0, 1])
        except Exception:
            pass
        print("")
        print("  info: S_rsm mean=%.3e%s"
              % (s.mean(), "" if corr is None else "  corr(diff, E/pi*albedo)=%.3f" % corr))
        verdict("RSM source reaches the HDR (S_rsm > 0)", s.mean() > S_RSM_MIN,
                "S_rsm mean=%.3e (> %.0e)" % (s.mean(), S_RSM_MIN))
    except SystemExit as e:
        print("  info: S_rsm unavailable (%s)" % e)
        failures.append("S_rsm measurable")

    if failures:
        print("")
        print("!! %d verdict(s) failed: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("")
    print("all verdicts passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
