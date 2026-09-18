"""forward_stack_check.py -- assertions for Tools/gi/forward_stack_check.ps1 (task 26 / 9.2-H).

The four runs differ only in the FORWARD diffuse stack (empty, {IBL}, {RSM}, {IBL,RSM}).
It asserts:

  1. the reading depends on the stack at all (pairwise distinguishable) -- before task 26 the
     cfg keys were applied to the Deferred pipeline only, so `pipeline_mode=0` ignored them;
  2. adding a source does not brighten: mean({IBL,RSM}) < mean({IBL}) (the old hardcoded code
     summed IBL + RSM, so turning RSM on could only raise the reading);
  3. the two-source reading is the WEIGHTED MEAN of the single-source readings, not the sum:
     with equal weights mean({IBL,RSM}) == (mean({IBL}) + mean({RSM})) / 2.

It also REPORTS S_rsm_forward = mean({RSM}) - mean(empty): the three criteria above all hold
when a source contributes exactly zero, and that is precisely the situation measured here
(defect 9.2-AD / task 34: Forward's RSM pass never registers, so the {RSM} run is the empty
run). Reported, not asserted -- see the note in the .ps1 header.

Exit code 0 = all assertions pass.
"""
import os
import sys

import numpy as np

MEAN_TOL = 0.02      # relative tolerance for the weighted-mean identity (2%)
DISTINCT_TOL = 0.001  # two readings must differ by at least 0.1% to count as "stack matters"
S_RSM_MIN = 1e-6     # below this the source is "not reaching the HDR" (reported only)


def hdr_mean(directory, tag):
    path = os.path.join(directory, "gi_%s_hdr.f16" % tag)
    a = np.fromfile(path, dtype=np.float16).astype(np.float32)
    if a.size % 4 != 0:
        raise SystemExit("unexpected dump size: %s" % path)
    a = a.reshape(-1, 4)
    return float((0.2126 * a[:, 0] + 0.7152 * a[:, 1] + 0.0722 * a[:, 2]).mean())


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

    # ---- 报告项：这个源到底有没有进画面（判据看不见的那件事）-------------------
    if m["fwd_none"] is not None:
        s_rsm = rsm - m["fwd_none"]
        print("")
        if abs(s_rsm) <= S_RSM_MIN:
            print("  [KNOWN] the RSM source does NOT reach the Forward HDR:")
            print("          S_rsm = mean({RSM}) - mean(empty) = %.3e (<= %.0e)"
                  % (s_rsm, S_RSM_MIN))
            print("          {RSM} %.7f vs empty %.7f -- the three criteria above cannot see"
                  % (rsm, m["fwd_none"]))
            print("          this (they all hold for an identically-zero source).")
            print("          Cause: defect 9.2-AD / task 34 (Forward's RSM pass never")
            print("          registers: the sample does not drive Forward's shadow system,")
            print("          and the pass reads a CSM light VP that the frame graph does not")
            print("          order before it). Reported, not asserted.")
        else:
            print("  info: S_rsm (Forward) = %.3e (the source reaches the HDR)" % s_rsm)

    if failures:
        print("")
        print("!! %d verdict(s) failed: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("")
    print("all verdicts passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
