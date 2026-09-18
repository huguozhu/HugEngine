"""ibl_lut_gate_check.py -- assertions for Tools/gi/ibl_lut_gate_check.ps1 (9.2-X / task 27).

Three configurations, two of them with an EMPTY specular stack (the trigger for the defect):

  iblgate_empty : diffuse empty, specular empty, ao empty  (the original reproduction)
  iblgate_ssao  : same, but ao = { SSAO }                  (the doc's third case)
  iblgate_ibl   : specular = { IBL }                       (control: always baked)

Assertions:
  1. the HDR maximum stays in the sane range for EVERY case. Before the fix the two
     empty-specular cases read max = 463.32 (uninitialised BRDF LUT sampled by the
     direct-light BRDF) while the control read 42.20;
  2. the two empty-specular cases agree with each other (an AO source must not matter);
  3. the structural half lives in the .ps1 output: the `IBL_Bake` pass must be registered
     in these configurations (the .ps1 prints the count for each case).

Exit code 0 = all assertions pass.
"""
import os
import sys

import numpy as np

MAX_OK = 100.0        # sane HDR max (measured 42.20); the defect read 463.32
AGREE_TOL = 0.02      # the two empty-specular cases must agree within 2%


def hdr_stats(directory, tag):
    path = os.path.join(directory, "gi_%s_hdr.f16" % tag)
    a = np.fromfile(path, dtype=np.float16).astype(np.float32)
    if a.size % 4 != 0:
        raise SystemExit("unexpected dump size: %s" % path)
    a = a.reshape(-1, 4)
    lum = 0.2126 * a[:, 0] + 0.7152 * a[:, 1] + 0.0722 * a[:, 2]
    return float(lum.mean()), float(lum.max())


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else "."
    failures = []

    def verdict(name, ok, detail):
        print("  [%s] %s -- %s" % ("PASS" if ok else "FAIL", name, detail))
        if not ok:
            failures.append(name)

    stats = {}
    print("=== HDR reading per configuration ===")
    for tag in ("iblgate_empty", "iblgate_ssao", "iblgate_ibl"):
        st = hdr_stats(directory, tag)
        stats[tag] = st
        print("  %-14s mean=%.4f max=%.2f" % (tag, st[0], st[1]))

    print("")
    print("=== verdicts ===")
    for tag in ("iblgate_empty", "iblgate_ssao", "iblgate_ibl"):
        mx = stats[tag][1]
        verdict("%s has a sane HDR max" % tag, mx <= MAX_OK,
                "max=%.2f (<= %.0f; the defect read 463.32)" % (mx, MAX_OK))

    e, s = stats["iblgate_empty"], stats["iblgate_ssao"]
    rel = abs(e[1] - s[1]) / max(abs(e[1]), abs(s[1]), 1e-12)
    verdict("an AO source does not change the empty-specular picture", rel <= AGREE_TOL,
            "max %.2f vs %.2f (relative %.3f%%)" % (e[1], s[1], rel * 100.0))

    if failures:
        print("")
        print("!! %d verdict(s) failed: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("")
    print("all verdicts passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
