"""forward_stack_check.py -- assertions for Tools/gi/forward_stack_check.ps1 (task 26 / 9.2-H).

The three runs differ only in the FORWARD diffuse stack ({IBL}, {RSM}, {IBL,RSM}). It asserts:

  1. the reading depends on the stack at all (pairwise distinguishable) -- before task 26 the
     cfg keys were applied to the Deferred pipeline only, so `pipeline_mode=0` ignored them;
  2. adding a source does not brighten: mean({IBL,RSM}) < mean({IBL}) (the old hardcoded code
     summed IBL + RSM, so turning RSM on could only raise the reading);
  3. the two-source reading is the WEIGHTED MEAN of the single-source readings, not the sum:
     with equal weights mean({IBL,RSM}) == (mean({IBL}) + mean({RSM})) / 2.

Exit code 0 = all assertions pass.
"""
import os
import sys

import numpy as np

MEAN_TOL = 0.02      # relative tolerance for the weighted-mean identity (2%)
DISTINCT_TOL = 0.001  # two readings must differ by at least 0.1% to count as "stack matters"


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
    for tag in ("fwd_ibl", "fwd_rsm", "fwd_ibl_rsm"):
        m[tag] = hdr_mean(directory, tag)
        print("  %-12s mean=%.7f" % (tag, m[tag]))

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

    if failures:
        print("")
        print("!! %d verdict(s) failed: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("")
    print("all verdicts passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
