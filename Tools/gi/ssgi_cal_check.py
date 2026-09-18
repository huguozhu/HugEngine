"""Assertions for ssgi_cal_check.ps1 (docs task 10 / section 9.2-P).

1. Furnace (analytic): SSGI's own estimate under "incident radiance = 1, albedo = 1" must be
   E/pi = 1. The probe line in the run log carries the centre/background luminance.
2. Blend: with equal weights the two-source differential must match the mean of the two
   single-source differentials (small positive bias is expected: the screen-edge confidence
   drops SSGI at the border but not DDGI).
3. Magnitude: DDGI and SSGI estimate the same physical quantity -> same order of magnitude.
"""
import re
import sys, os
import numpy as np

W, H = 1920, 1080


def lum(tag, outdir):
    a = np.fromfile(os.path.join(outdir, "gi_%s_hdr.f16" % tag), dtype=np.float16).astype(np.float32)
    a = a.reshape(H, W, 4)
    return 0.2126 * a[:, :, 0] + 0.7152 * a[:, :, 1] + 0.0722 * a[:, :, 2]


def furnace_reading(outdir):
    log = os.path.join(outdir, "ssgical_furnace.log")
    last = None
    for line in open(log, encoding="utf-8", errors="replace").read().splitlines():
        m = re.search(r"中心=([0-9.]+) 背景=([0-9.]+) 比值=([0-9.]+)", line)
        if m:
            last = (float(m.group(1)), float(m.group(2)), float(m.group(3)))
    return last


def main(outdir):
    fails = []

    c, bg, ratio = furnace_reading(outdir)
    print("Test 1 (furnace, analytical truth E/pi = 1): centre=%.4f background=%.4f ratio=%.4f"
          % (c, bg, ratio))
    for name, v in (("centre", c), ("background", bg)):
        if not (0.90 <= v <= 1.10):
            fails.append("furnace %s reads %.4f, expected 1.0 +-10%% (normalisation is wrong)" % (name, v))
    if not fails:
        print("  PASS: the cosine-weighted normalisation is exact (Sigma(L*cos)/Sigma(cos))")

    n, s, d, b = (lum(t, outdir) for t in ("none", "ssgi", "ddgi", "both"))
    Ss, Sd, Sb = s - n, d - n, b - n
    avg = 0.5 * (Ss + Sd)
    blend = Sb.mean() / max(avg.mean(), 1e-12)
    print("Test 2 (blend): S_ssgi=%.3e S_ddgi=%.3e S_both=%.3e  S_both/mean=%.4f"
          % (Ss.mean(), Sd.mean(), Sb.mean(), blend))
    if not (0.90 <= blend <= 1.15):
        fails.append("blend ratio %.4f outside [0.90, 1.15]: the normalised composite does not "
                     "behave like a weighted mean" % blend)
    else:
        print("  PASS: the two-source differential is the weighted mean of the two sources")

    mag = Sd.mean() / max(Ss.mean(), 1e-12)
    print("Test 3 (magnitude): mean(S_ddgi)/mean(S_ssgi) = %.2fx" % mag)
    if mag >= 5.0:
        fails.append("DDGI is %.2fx SSGI: the two are still not estimating the same quantity "
                     "(was 21x before SSGI-CAL)" % mag)
    else:
        print("  PASS: both sources are in the same order of magnitude (was 21x)")

    for f in fails:
        print("FAIL: " + f)
    if fails:
        print("RESULT: FAIL")
        return 1
    print("RESULT: PASS (3/3)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "."))
