"""repeatability_check.py -- assertions for Tools/gi/repeatability_check.ps1 (9.2-Y / task 28).

Reads `gi_rep<1..N>_hdr.f16` (the same byte-identical config, N runs) and asserts:

  1. all N readings agree within SPREAD_TOL (the docs' criterion: the same config must give the
     same reading -- a split into groups is exactly what 9.2-Y was about);
  2. the reading sits in the EMPTY-diffuse-stack range, not in the IBL-back-filled range:
     9.2-Y's two groups were ~0.05773 (stack stayed empty) and ~0.07554 (something refilled it
     with IBL). An empty diffuse channel means "no indirect diffuse" (docs 3.1), so a reading
     above EMPTY_MAX means the stack was changed behind our back.

The config-file half (the sample must not rewrite the file) is asserted in the .ps1.

Exit code 0 = all assertions pass.
"""
import os
import sys

import numpy as np

SPREAD_TOL = 0.005      # 0.5% between the largest and smallest reading
EMPTY_MAX = 0.065       # above this the diffuse stack was back-filled with IBL (9.2-Y group 2)


def hdr_mean(directory, index):
    path = os.path.join(directory, "gi_rep%d_hdr.f16" % index)
    a = np.fromfile(path, dtype=np.float16).astype(np.float32)
    if a.size % 4 != 0:
        raise SystemExit("unexpected dump size: %s" % path)
    a = a.reshape(-1, 4)
    return float((0.2126 * a[:, 0] + 0.7152 * a[:, 1] + 0.0722 * a[:, 2]).mean())


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else "."
    runs = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    failures = []

    def verdict(name, ok, detail):
        print("  [%s] %s -- %s" % ("PASS" if ok else "FAIL", name, detail))
        if not ok:
            failures.append(name)

    print("=== identical config, %d runs ===" % runs)
    vals = []
    for i in range(1, runs + 1):
        v = hdr_mean(directory, i)
        vals.append(v)
        print("  run %d  mean=%.7f" % (i, v))

    lo, hi = min(vals), max(vals)
    rel = (hi - lo) / max(abs(lo), 1e-12)
    print("")
    print("=== verdicts ===")
    verdict("all runs agree on the reading", rel <= SPREAD_TOL,
            "spread %.4f%% (min %.7f, max %.7f; tolerance %.1f%%)"
            % (rel * 100.0, lo, hi, SPREAD_TOL * 100.0))
    verdict("the empty diffuse stack stayed empty", hi <= EMPTY_MAX,
            "max reading %.7f (<= %.4f; the IBL-back-filled group was ~0.07554)"
            % (hi, EMPTY_MAX))

    if failures:
        print("")
        print("!! %d verdict(s) failed: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("")
    print("all verdicts passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
