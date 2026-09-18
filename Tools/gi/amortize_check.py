"""Assertions for amortize_check.ps1 (docs task 12 / AMORTIZE).

S(stride, frame) = mean luminance of the DDGI contribution, i.e.
lum(HDR{diffuse=DDGI}) - lum(HDR{diffuse=empty}), both taken at the same frame and stride.

The probe field is driven by the temporal blend, so what matters is the NUMBER OF PROBE
UPDATES, not the number of frames: stride N needs N times as many frames for the same state.
Hence the assertions compare (stride N, frame N*k) against (stride 1, frame k).
"""
import sys, os
import numpy as np

W, H = 1920, 1080


def lum(tag, outdir):
    a = np.fromfile(os.path.join(outdir, "gi_%s_hdr.f16" % tag), dtype=np.float16).astype(np.float32)
    a = a.reshape(H, W, 4)
    return 0.2126 * a[:, :, 0] + 0.7152 * a[:, :, 1] + 0.0722 * a[:, :, 2]


def contribution(outdir, stride, frame):
    n = lum("amort_s%d_f%d_none" % (stride, frame), outdir)
    d = lum("amort_s%d_f%d_ddgi" % (stride, frame), outdir)
    return float((d - n).mean())


def main(outdir):
    fails = []
    print("DDGI contribution S (mean luminance of HDR{DDGI} - HDR{empty}):")
    print("  %-16s %-16s %-10s %s" % ("stride 1", "stride 4", "ratio", "verdict"))
    for k in (30, 60, 120):
        s1 = contribution(outdir, 1, k)
        s4 = contribution(outdir, 4, 4 * k)
        ratio = s4 / s1 if s1 != 0 else float("nan")
        ok = abs(ratio - 1.0) <= 0.15
        if not ok:
            fails.append("S(4,%d)/S(1,%d) = %.3f, outside [0.85, 1.15]: the amortised update "
                         "does not reproduce the same probe field at equal update count"
                         % (4 * k, k, ratio))
        print("  f=%-3d %-10.6f f=%-4d %-10.6f %-10.3f %s"
              % (k, s1, 4 * k, s4, ratio, "OK" if ok else "FAIL"))

    # The default (stride 1) must still be the established baseline, and stride 4 must be a
    # pure "slower convergence" trade: compare S at the same frame to show it is lower
    # (not yet converged), which is the expected, measurable cost of amortisation.
    s1_120 = contribution(outdir, 1, 120)
    s4_120 = contribution(outdir, 4, 120)
    print("cost of amortisation: S(1,120) = %.6f  vs  S(4,120) = %.6f  (%.0f%%)"
          % (s1_120, s4_120, 100.0 * s4_120 / s1_120))
    if not (s4_120 < s1_120):
        fails.append("S(4,120) is not below S(1,120): the amortised run looks converged "
                     "already, which is suspicious (probes may not actually be skipped)")

    for f in fails:
        print("FAIL: " + f)
    if fails:
        print("RESULT: FAIL")
        return 1
    print("RESULT: PASS (equal update count -> equal contribution; slower convergence is the cost)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "."))
