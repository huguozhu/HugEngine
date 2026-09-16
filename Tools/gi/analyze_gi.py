"""Sanity + magnitude check on GI sampling dumps produced by dump_gi.ps1.

Verifies the three runs are actually comparable (identical albedo), isolates each source
by differencing against the empty-stack baseline, and reports magnitudes. Use
p5_spectrum.py for the band/overlap analysis.

    S_x = lum(HDR_x - HDR_none)          (direct light / sky / specular cancel exactly)

Raw files are RGBA16F, tightly packed, no header: H*W*4 float16 values, dimensions from
the companion _meta.txt.
"""
import os

import numpy as np

# <repo>/Tools/gi/analyze_gi.py  ->  <repo>/Build/verify
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "Build", "verify")
TAGS = ["none", "ddgi", "ssgi"]


def meta(tag):
    return {p[0]: (int(p[1]), int(p[2]))
            for line in open(os.path.join(OUT, f"gi_{tag}_meta.txt"))
            for p in [line.split()]}


def load(tag, name):
    w, h = meta(tag)[name]
    a = np.fromfile(os.path.join(OUT, f"gi_{tag}_{name}.f16"), dtype="<f2")
    if a.size != w * h * 4:
        raise RuntimeError(f"gi_{tag}_{name}.f16: {a.size} values, expected {w*h*4}")
    return a.reshape(h, w, 4).astype(np.float64)


def lum(c):
    return 0.2126 * c[..., 0] + 0.7152 * c[..., 1] + 0.0722 * c[..., 2]


imgs = {t: (load(t, "hdr"), load(t, "albedo")) for t in TAGS}
base, alb = imgs["none"]
h, w = base.shape[:2]
valid = (alb[..., 0] > 0.02) & (alb[..., 1] > 0.02) & (alb[..., 2] > 0.02)
print(f"image {w}x{h}, valid (albedo>0.02) pixels {valid.sum()} ({100.0*valid.mean():.1f}%)")

for t in TAGS:
    c, a = imgs[t]
    L = lum(c)
    print(f"{t:5s} HDR lum mean={L.mean():.6g} max={L.max():.6g} "
          f"nan={np.isnan(c).sum()} inf={np.isinf(c).sum()}")
    print(f"      albedo mean={a[...,0].mean():.4f},{a[...,1].mean():.4f},{a[...,2].mean():.4f} "
          f"metal(alpha)={a[...,3].mean():.4f}")

# albedo must be IDENTICAL across runs, otherwise the runs are not comparable
for t in TAGS[1:]:
    d = np.abs(imgs[t][1] - imgs["none"][1]).max()
    flag = "OK" if d == 0 else "!! RUNS NOT COMPARABLE"
    print(f"albedo max |{t} - none| = {d:.6g}  {flag}")

print()
for t in ["ddgi", "ssgi"]:
    diff = imgs[t][0] - base
    d = lum(diff)
    e = lum(diff[..., :3] / np.maximum(alb[..., :3], 1e-3))   # divide out receiver albedo
    print(f"{t:5s} dLum  mean={d[valid].mean():.6g} std={d[valid].std():.6g}  "
          f"frac>1e-5={(d[valid]>1e-5).mean():.4f} frac<-1e-5={(d[valid]<-1e-5).mean():.4f}")
    print(f"      (diff/albedo) lum mean={e[valid].mean():.6g} p99.9={np.percentile(e[valid],99.9):.5f}")

lD = lum((imgs["ddgi"][0] - base)[..., :3] / np.maximum(alb[..., :3], 1e-3))
lS = lum((imgs["ssgi"][0] - base)[..., :3] / np.maximum(alb[..., :3], 1e-3))
if lS[valid].std() > 0 and lD[valid].std() > 0:
    print(f"\ncorr(ddgi, ssgi) over valid = {np.corrcoef(lD[valid], lS[valid])[0,1]:.4f}")
print(f"magnitude ratio mean(ddgi)/mean(ssgi) = "
      f"{lD[valid].mean()/max(lS[valid].mean(), 1e-30):.1f}x")
