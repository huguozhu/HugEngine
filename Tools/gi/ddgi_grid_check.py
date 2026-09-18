"""ddgi_grid_check.py -- analyse the dumps produced by ddgi_grid_check.ps1 (docs 9.2-K, task 14).

Reads gi_grid_<case>_{none,ddgi}_hdr.f16 from the given directory and reports the
DDGI screen contribution (lum(HDR_ddgi) - lum(HDR_none)) per case, then applies three
verdicts:

  fixed : the fixed 8x4x8 / cell-3 grid cannot cover the scene, so with the probe-grid
          confidence bit active the contribution must collapse to ~0.
  fit*  : after fitting the grid to the scene AABB the contribution must come back, and
          the different fitted resolutions must NOT agree: the probe field has to depend
          on the probe positions (task 17's evidence -- see the note below).

NOTE ON DIRECTION (this check asserted the opposite before task 17): task 14's measurement
found that the three fitted resolutions agreed to 0.0015% because the probe update never
used `samplePos` -- the probe field was uniform, i.e. the source was just "the IBL
irradiance as a function of direction". That finding is what task 17 fixed by tracing the
probe rays with hardware RT. The check therefore now asserts the fixed behaviour: the
resolutions must differ by a wide margin. The pre-fix numbers (spread 0.0015%) are kept in
the docs as the baseline of the defect.

Exit code 0 = all verdicts pass.
"""
import os
import sys

import numpy as np

W, H = 1920, 1080
ZERO_MAX = 1e-4        # "contribution collapsed" threshold (dLum mean)
BACK_MIN = 1e-2        # "contribution restored" threshold
SPREAD_MIN = 0.05      # fitted resolutions must differ by at least 5% (probe positions matter)


def load(directory, tag):
    path = os.path.join(directory, "gi_%s_hdr.f16" % tag)
    a = np.fromfile(path, dtype=np.float16).astype(np.float32)
    if a.size != W * H * 4:
        raise SystemExit("unexpected dump size for %s: %d" % (path, a.size))
    return a.reshape(H, W, 4)


def lum(x):
    return 0.2126 * x[..., 0] + 0.7152 * x[..., 1] + 0.0722 * x[..., 2]


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: ddgi_grid_check.py <dumpDir> <case> [<case> ...]")
    directory = sys.argv[1]
    cases = sys.argv[2:]

    diff = {}
    print("=== DDGI screen contribution per case (lum(HDR_ddgi) - lum(HDR_none)) ===")
    for case in cases:
        n = lum(load(directory, "grid_%s_none" % case))
        d = lum(load(directory, "grid_%s_ddgi" % case))
        delta = d - n
        diff[case] = float(delta.mean())
        print("  %-10s none=%.7f ddgi=%.7f  dLum mean=%.6e std=%.6f"
              % (case, n.mean(), d.mean(), delta.mean(), delta.std()))

    failures = []

    def verdict(name, ok, detail):
        print("  [%s] %s -- %s" % ("PASS" if ok else "FAIL", name, detail))
        if not ok:
            failures.append(name)

    print("")
    print("=== verdicts ===")
    if "gridFixed" in diff:
        verdict("fixed grid is gated out",
                abs(diff["gridFixed"]) < ZERO_MAX,
                "dLum=%.3e (< %.0e) => out-of-grid queries carried the whole old contribution"
                % (diff["gridFixed"], ZERO_MAX))
    fits = [c for c in cases if c.startswith("gridFit") and c in diff]
    for c in fits:
        verdict("fitted grid %s restores the contribution" % c,
                diff[c] > BACK_MIN,
                "dLum=%.6e (> %.0e)" % (diff[c], BACK_MIN))
    if len(fits) >= 2:
        values = [diff[c] for c in fits]
        spread = (max(values) - min(values)) / max(abs(sum(values) / len(values)), 1e-9)
        verdict("probe positions participate (resolutions differ)",
                spread >= SPREAD_MIN,
                "spread over %s = %.2f%% (>= %.0f%%) => the probe field depends on the grid"
                % ("/".join(fits), spread * 100.0, SPREAD_MIN * 100.0))

    if failures:
        print("")
        print("!! %d verdict(s) failed: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("")
    print("all verdicts passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
