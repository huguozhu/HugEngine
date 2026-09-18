"""Assertions for confidence_check.ps1 (docs section 3.2 / task 9).

Reads the four HDR dumps and checks that the per-pixel confidence multiplier really
re-weights the normalised blend, and that the fade band is driven by the UBO:

  Test 1 (band 5%):  |HDR{IBL} - HDR{IBL,SSGI}| over the outermost pixel ring must be a tiny
                     fraction of the centre value (confidence ~0 at the border), while the
                     centre value must stay clearly positive (SSGI is not dead).
  Test 2 (band 50%): the ring 5%..15% from the edge is fully trusted at band 5% but only
                     ~10..30% trusted at band 50%, so |A-B| there must drop well below the
                     band-5% value -- the quantitative signature of a data-driven band.

Comparisons are luminance means, so run-to-run jitter (~1e-6 on the mean) cannot flip the
verdict; thresholds are 1e-2..1e-3.
"""
import sys, os
import numpy as np

W, H = 1920, 1080


def load_hdr_lum(outdir, tag):
    a = np.fromfile(os.path.join(outdir, "gi_%s_hdr.f16" % tag), dtype=np.float16).astype(np.float32)
    a = a.reshape(H, W, 4)
    return 0.2126 * a[:, :, 0] + 0.7152 * a[:, :, 1] + 0.0722 * a[:, :, 2]


def edge_distance():
    yy, xx = np.mgrid[0:H, 0:W]
    u = (xx + 0.5) / W
    v = (yy + 0.5) / H
    return np.minimum(np.minimum(u, 1.0 - u), np.minimum(v, 1.0 - v))


def main(outdir):
    A = load_hdr_lum(outdir, "c1_ibl")
    B = load_hdr_lum(outdir, "c1_ibl_ssgi")
    Aw = load_hdr_lum(outdir, "c3_ibl_wide")
    Bw = load_hdr_lum(outdir, "c3_ibl_ssgi_wide")
    e = edge_distance()

    centre = (np.abs((np.mgrid[0:H, 0:W][1] + 0.5) / W - 0.5) <= 0.25) & \
             (np.abs((np.mgrid[0:H, 0:W][0] + 0.5) / H - 0.5) <= 0.25)
    border = e <= 1.0 / min(W, H)          # outermost pixel ring
    ring = (e > 0.05) & (e <= 0.15)        # ring used for the band comparison

    fails = []
    d = np.abs(A - B)
    d_border = float(d[border].mean())
    d_centre = float(d[centre].mean())
    print("Test 1 (band 5%%): |A-B| border(1px) = %.3e   centre = %.3e   ratio = %.4f"
          % (d_border, d_centre, d_border / max(d_centre, 1e-12)))
    if d_centre <= 1e-3:
        fails.append("centre: SSGI contributes nothing even where it is trusted (dead source)")
    if d_border > 0.10 * d_centre:
        fails.append("border: SSGI still contributes %.4f (%.1f%% of centre) although its "
                     "confidence is ~0" % (d_border, 100.0 * d_border / max(d_centre, 1e-12)))

    dw = np.abs(Aw - Bw)
    r_narrow = float(d[ring].mean())
    r_wide = float(dw[ring].mean())
    ratio = r_wide / max(r_narrow, 1e-12)
    print("Test 2 (band 5%% -> 50%%): ring 5%%..15%% |A-B| = %.3e -> %.3e   ratio = %.3f"
          % (r_narrow, r_wide, ratio))
    if r_narrow <= 1e-3:
        fails.append("ring: no signal at band 5%, cannot compare bands")
    elif ratio > 0.40:
        fails.append("ring: widening the band to 50%% barely changed the contribution "
                     "(ratio %.3f) -- the band is not UBO-driven" % ratio)

    for f in fails:
        print("FAIL: " + f)
    if fails:
        print("RESULT: FAIL")
        return 1
    print("RESULT: PASS (2/2)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "."))
