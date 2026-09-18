"""forward_stack_check.py -- assertions for Tools/gi/forward_stack_check.ps1 (task 26 / 9.2-H).

The four runs differ only in the FORWARD diffuse stack (empty, {IBL}, {RSM}, {IBL,RSM}).
It asserts:

  1. the reading depends on the stack at all (pairwise distinguishable) -- before task 26 the
     cfg keys were applied to the Deferred pipeline only, so `pipeline_mode=0` ignored them;
  2. adding a source does not brighten: mean({IBL,RSM}) < mean({IBL}) (the old hardcoded code
     summed IBL + RSM, so turning RSM on could only raise the reading);
  3. the two-source reading is the WEIGHTED MEAN of the single-source readings, not the sum:
     with equal weights mean({IBL,RSM}) == (mean({IBL}) + mean({RSM})) / 2.
  4. the RSM source actually REACHES the Forward HDR: S_rsm_forward = mean({RSM}) - mean(empty)
     must be > 1e-6. Task 26 could not assert this -- all three criteria above hold for an
     identically-zero source, and that was exactly the situation then (defect 9.2-AD: Forward's
     RSM had no producer). It was reported only, on purpose, so the open defect would not be
     frozen into the suite; task 34 fixed the producer, so it is asserted now.
  5. the RSM chain behind it is genuinely producing: the three dumped maps of the {RSM} run
     (position / encoded normal / VPL radiance) must all hold geometry (same per-stage
     thresholds as the Deferred-side `rsm_indirect_check`).
  6. the source is VIEW-INDEPENDENT: `fwd_rsm_view` is the {RSM} run with the camera moved 300
     along x, and its three RSM maps must be byte-identical to `fwd_rsm`'s. The fixed frustum is
     fitted to the scene bounds + light direction only, so a world-space source has to come out
     identical; the old CSM-cascade-0 VP (fitted to the camera frustum) could not satisfy this.

Exit code 0 = all assertions pass.
"""
import os
import sys

import numpy as np

MEAN_TOL = 0.02      # relative tolerance for the weighted-mean identity (2%)
DISTINCT_TOL = 0.001  # two readings must differ by at least 0.1% to count as "stack matters"
S_RSM_MIN = 1e-6     # below this the source is "not reaching the HDR"
POS_COVER_MIN = 0.20   # the RSM position map must actually cover geometry
RAD_NONZERO_MIN = 0.20  # ...and the VPL radiance map must too
RAD_MEAN_MIN = 0.005    # ...at a real radiance, not numerical dust
NRM_COVER_TOL = 0.02    # normal coverage must match position coverage (same pass, same texels)
VIEW_IDENT_MIN = 0.9999  # RSM maps must be identical across cameras (world-space source)
VIEW_MAXDIFF = 1e-3      # ...with a tolerance of one 16-bit step for a stray texel

W, H = 1920, 1080


def hdr_mean(directory, tag):
    path = os.path.join(directory, "gi_%s_hdr.f16" % tag)
    a = np.fromfile(path, dtype=np.float16).astype(np.float32)
    if a.size % 4 != 0:
        raise SystemExit("unexpected dump size: %s" % path)
    a = a.reshape(-1, 4)
    return float((0.2126 * a[:, 0] + 0.7152 * a[:, 1] + 0.0722 * a[:, 2]).mean())


def rsm_map(directory, tag, suffix):
    """RSM target of a run: square, so the side is the square root of the pixel count."""
    path = os.path.join(directory, "gi_%s_%s.f16" % (tag, suffix))
    a = np.fromfile(path, dtype=np.float16).astype(np.float32)
    px = a.size // 4
    side = int(px ** 0.5)
    if side * side != px:
        raise SystemExit("not a square RSM dump: %s" % path)
    return a.reshape(side, side, 4)


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else "."
    failures = []

    def verdict(name, ok, detail):
        print("  [%s] %s -- %s" % ("PASS" if ok else "FAIL", name, detail))
        if not ok:
            failures.append(name)

    m = {}
    print("=== Forward HDR reading per diffuse stack ===")
    for tag in ("fwd_none", "fwd_ibl", "fwd_rsm", "fwd_ibl_rsm"):
        try:
            m[tag] = hdr_mean(directory, tag)
            print("  %-12s mean=%.7f" % (tag, m[tag]))
        except OSError:
            m[tag] = None
            print("  %-12s (no dump)" % tag)

    ibl, rsm, both = m["fwd_ibl"], m["fwd_rsm"], m["fwd_ibl_rsm"]
    print("")
    print("=== verdicts ===")

    def rel(a, b):
        return abs(a - b) / max(abs(a), abs(b), 1e-12)

    distinct = (rel(ibl, rsm) > DISTINCT_TOL and rel(ibl, both) > DISTINCT_TOL
                and rel(rsm, both) > DISTINCT_TOL)
    verdict("the Forward picture depends on the layer stack", distinct,
            "IBL %.7f / RSM %.7f / both %.7f (pairwise > %.1f%%)"
            % (ibl, rsm, both, DISTINCT_TOL * 100.0))
    verdict("adding RSM does not brighten", both < ibl,
            "both %.7f < IBL %.7f (the old code summed the two sources)" % (both, ibl))

    expected = (ibl + rsm) / 2.0
    err = abs(both - expected) / max(abs(expected), 1e-12)
    verdict("two-source reading is the weighted mean, not the sum", err <= MEAN_TOL,
            "both %.7f vs (IBL+RSM)/2 = %.7f (relative %.3f%%, tolerance %.1f%%)"
            % (both, expected, err * 100.0, MEAN_TOL * 100.0))

    # ---- 任务 34：这个源真的进了画面（三条判据对"恒为 0 的源"全部成立）---------------
    if m["fwd_none"] is None:
        verdict("empty-stack control run present", False,
                "gi_fwd_none_hdr.f16 missing -- S_rsm cannot be measured")
    else:
        s_rsm = rsm - m["fwd_none"]
        verdict("the RSM source reaches the Forward HDR", s_rsm > S_RSM_MIN,
                "S_rsm = mean({RSM}) - mean(empty) = %.3e (> %.0e; it was exactly 0 before "
                "task 34, now %.3f%% of the screen mean)"
                % (s_rsm, S_RSM_MIN, 100.0 * s_rsm / max(rsm, 1e-12)))

    # ---- 任务 34：链路逐级真的产出（与 Deferred 侧 rsm_indirect_check 同一套阈值）------
    try:
        pos = rsm_map(directory, "fwd_rsm", "rsm_pos")
        nrm = rsm_map(directory, "fwd_rsm", "rsm_nrm")
        rad = rsm_map(directory, "fwd_rsm", "rsm_rad")
    except OSError as e:
        print("  !! missing Forward RSM chain dump: %s" % e)
        failures.append("Forward RSM chain dumps present")
        pos = nrm = rad = None
    if pos is not None:
        pos_cov = (np.abs(pos[..., 0]) + np.abs(pos[..., 1]) + np.abs(pos[..., 2])) > 0
        # 未覆盖的 texel 是清除值 (0,0,0,1)；覆盖的 texel 编码后三分量至少有一个 >= 0.21
        # （单位法线的分量不可能三个都接近 −1）——【不要用 |c-0.5|】，清除值 0 与 0.5 的差
        # 也是 0.5，会把"没覆盖"判成"覆盖"（Deferred 侧踩过一次）。
        nrm_cov = nrm[..., :3].sum(axis=2) > 0.05
        rad_lum = 0.2126 * rad[..., 0] + 0.7152 * rad[..., 1] + 0.0722 * rad[..., 2]
        print("")
        print("  dumps({RSM} run): rsm_pos covered=%.2f%%  rsm_nrm covered=%.2f%%  "
              "rsm_rad nonzero=%.2f%% mean=%.4f"
              % (100.0 * pos_cov.mean(), 100.0 * nrm_cov.mean(),
                 100.0 * (rad_lum > 0).mean(), rad_lum.mean()))
        verdict("Forward RSM position map holds geometry", pos_cov.mean() > POS_COVER_MIN,
                "covered=%.2f%% (>= %.0f%%)" % (100.0 * pos_cov.mean(), 100.0 * POS_COVER_MIN))
        verdict("Forward RSM normal map covers the same texels",
                abs(nrm_cov.mean() - pos_cov.mean()) < NRM_COVER_TOL,
                "nrm=%.2f%% vs pos=%.2f%%" % (100.0 * nrm_cov.mean(), 100.0 * pos_cov.mean()))
        verdict("Forward VPL radiance map produces real radiance",
                (rad_lum > 0).mean() > RAD_NONZERO_MIN and rad_lum.mean() > RAD_MEAN_MIN,
                "nonzero=%.2f%% (>= %.0f%%) mean=%.4f (>= %.3f)"
                % (100.0 * (rad_lum > 0).mean(), 100.0 * RAD_NONZERO_MIN,
                   rad_lum.mean(), RAD_MEAN_MIN))

    # ---- 任务 34：世界空间源必须视角无关（相机挪 300 后三张图逐字节相同）--------------
    try:
        print("")
        for suffix in ("rsm_pos", "rsm_nrm", "rsm_rad"):
            a = rsm_map(directory, "fwd_rsm", suffix)
            b = rsm_map(directory, "fwd_rsm_view", suffix)
            ident = float((a == b).mean())
            maxdiff = float(np.abs(a - b).max())
            verdict("RSM map %s is view-independent" % suffix,
                    ident >= VIEW_IDENT_MIN and maxdiff <= VIEW_MAXDIFF,
                    "identical=%.4f%% (>= %.2f%%) max|diff|=%.3e (<= %.0e)"
                    % (100.0 * ident, 100.0 * VIEW_IDENT_MIN, maxdiff, VIEW_MAXDIFF))
    except OSError as e:
        print("  !! missing view-independence dump: %s" % e)
        failures.append("RSM view-independence dumps present")

    if failures:
        print("")
        print("!! %d verdict(s) failed: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("")
    print("all verdicts passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
