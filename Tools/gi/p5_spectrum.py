"""P5: does LowPass(SSGI) approximate DDGI?

Answers whether DDGI and SSGI overlap enough in the *same* spatial band to justify
frequency separation, or whether they are better treated as alternatives (normalized,
the current production mode) or as disjoint sources (additive).

Isolates each source by differencing single-source runs produced by dump_gi.ps1
(identical configs except the diffuse channel stack, so direct light / sky / specular
cancel exactly):

    S_x = lum(HDR_x - HDR_none)

then compares band content and tests whether any low-pass of SSGI reproduces DDGI.

The analysis runs twice, and the second pass is the important one:
  mask "all"   - every pixel with usable albedo
  mask "tight" - additionally restricted to SSGI's own coverage, eroded. SSGI returns
                 exactly 0 where its screen-space rays find nothing; that 0 / non-zero
                 boundary is a hard edge which manufactures broadband energy and would
                 otherwise be mistaken for genuine high-frequency GI. On the "all" mask
                 SSGI looks broadband; on the "tight" mask both sources turn out to have
                 almost the same spectrum, which is the actual finding.

Conclusion recorded in docs: no low-pass scale makes LowPass(SSGI) approach DDGI
(correlation falls monotonically with stronger low-passing), so frequency separation was
dropped and the normalized blend kept as-is.
"""
import os

import numpy as np

# <repo>/Tools/gi/p5_spectrum.py  ->  <repo>/Build/verify
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "Build", "verify")


def meta(tag):
    return {p[0]: (int(p[1]), int(p[2]))
            for line in open(os.path.join(OUT, f"gi_{tag}_meta.txt"))
            for p in [line.split()]}


def load(tag, name):
    w, h = meta(tag)[name]
    return np.fromfile(os.path.join(OUT, f"gi_{tag}_{name}.f16"),
                       dtype="<f2").reshape(h, w, 4).astype(np.float64)


def lum(c):
    return 0.2126 * c[..., 0] + 0.7152 * c[..., 1] + 0.0722 * c[..., 2]


base = load("none", "hdr")
alb = load("none", "albedo")
h, w = base.shape[:2]
valid = (alb[..., 0] > 0.02) & (alb[..., 1] > 0.02) & (alb[..., 2] > 0.02)
S = {t: lum(load(t, "hdr") - base) for t in ("ddgi", "ssgi")}


def erode_block(mask, k):
    """Erode by a k x k block structuring element (avoids a scipy dependency)."""
    hh, ww = (mask.shape[0] // k) * k, (mask.shape[1] // k) * k
    m = mask[:hh, :ww].reshape(hh // k, k, ww // k, k).all(axis=(1, 3))
    out = np.zeros_like(mask)
    out[:hh, :ww] = np.repeat(np.repeat(m, k, axis=0), k, axis=1)
    return out


covered = S["ssgi"] > 1e-6
MASKS = [
    ("all  ", valid),
    ("tight", valid & erode_block(covered, 16)),
]

fr = np.sqrt(np.fft.fftfreq(h)[:, None] ** 2 + np.fft.fftfreq(w)[None, :] ** 2)
wy, wx = np.hanning(h)[:, None], np.hanning(w)[None, :]
NB = 48
edges = np.linspace(0.0, 0.5, NB + 1)


def band_shares(F):
    P = np.abs(F) ** 2
    idx = np.clip(np.digitize(fr.ravel(), edges) - 1, 0, NB - 1)
    tot = np.bincount(idx, weights=P.ravel(), minlength=NB)
    cnt = np.bincount(idx, minlength=NB)
    p = tot / np.maximum(cnt, 1)
    return p / p.sum()


def corr(x, y, m):
    a, b = x[m].copy(), y[m].copy()
    a -= a.mean()
    b -= b.mean()
    d = np.sqrt((a * a).sum() * (b * b).sum())
    return float((a * b).sum() / d) if d > 0 else float("nan")


for label, mask in MASKS:
    print("=" * 74)
    print(f"mask = {label}   pixels = {mask.sum()} ({100.0*mask.mean():.1f}%)")
    print("=" * 74)
    W = (wy * wx) * mask
    A = S["ddgi"] * W
    B = S["ssgi"] * W
    A -= A[mask].mean() * W
    B -= B[mask].mean() * W
    FA, FB = np.fft.fft2(A), np.fft.fft2(B)
    pA, pB = band_shares(FA), band_shares(FB)

    print(f"  S_ddgi  mean={S['ddgi'][mask].mean():.6g} std={S['ddgi'][mask].std():.6g}")
    print(f"  S_ssgi  mean={S['ssgi'][mask].mean():.6g} std={S['ssgi'][mask].std():.6g}")
    print(f"  magnitude ratio DDGI/SSGI = "
          f"{S['ddgi'][mask].mean()/max(S['ssgi'][mask].mean(),1e-30):.1f}x")
    print("\n  band(cyc/px)   DDGI share   SSGI share    D/S")
    for i in range(0, NB, 4):
        sa, sb = pA[i:i + 4].sum(), pB[i:i + 4].sum()
        print(f"   {edges[i]:.3f}-{edges[min(i+4,NB)]:.3f}   {100*sa:8.2f}%   "
              f"{100*sb:8.2f}%  {sa/max(sb,1e-30):7.2f}")

    print(f"\n  corr(SSGI, DDGI) no low-pass = {corr(B, A, mask):.4f}")
    print("   sigma[px]  gain   corr(LP(SSGI),DDGI)  corr(HP(SSGI),DDGI)  resid%")
    Ad = A[mask] - A[mask].mean()
    for sigma in (1, 2, 4, 8, 16, 32, 64):
        G = np.exp(-2.0 * (np.pi ** 2) * (sigma ** 2) * (fr ** 2))
        Blp = np.real(np.fft.ifft2(FB * G))
        Bhp = B - Blp
        bl = Blp[mask] - Blp[mask].mean()
        gain = float((Ad * bl).sum() / max((bl * bl).sum(), 1e-30))
        resid = Ad - gain * bl
        rel = float(np.sqrt((resid ** 2).mean()) / max(np.sqrt((Ad ** 2).mean()), 1e-30))
        print(f"   {sigma:8d}  {gain:5.2f}   {corr(Blp, A, mask):18.4f}   "
              f"{corr(Bhp, A, mask):18.4f}   {100*rel:5.1f}%")
    print()

print("Reading the sweep:")
print("  corr(LP) never rising above corr(no-LP)  -> no band of SSGI looks like DDGI")
print("  resid%                                   -> DDGI variance left unexplained")
print("                                              after best-fit scaling of LP(SSGI)")
