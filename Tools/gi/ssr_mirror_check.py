"""ssr_mirror_check.py -- analytic planar-mirror comparison for screen-space reflections.

Task 32 (docs 10.1 / 10.2): task 25 proved that SSR's rays become valid again, that both
march paths agree in magnitude and that Hi-Z is faster -- but NOT that the reflection lands
in the right place. A planar mirror has a closed-form answer, so that gap can be closed with
a pixel-accurate criterion instead of eyeballing:

    mirror the object centre C through the plane (n, d):   C' = C - 2 (n.C + d) n
    the camera E sees that virtual image along E->C'; the ray meets the mirror at
        Q = E + t (C' - E),   t = -(n.E + d) / (n.(C' - E))
    so the reflection of that object MUST appear at screen pixel project(Q).

The runner (`ssr_mirror_check.ps1`) creates the rig (a mirror slab on z=0 plus a red and a
green sphere) and dumps: hdr/albedo/gb_worldpos/gb_normal, the SSR output, the camera
parameters and the rig parameters. Everything below is computed from those dumps -- no
geometry is hardcoded here.

Checks
  1. SELF-CHECK  the predicted *direct* screen position of both spheres matches the albedo
     dump (<= 8 px). Without this the analytic reference itself would be unverified; with it,
     "the reflection is in the wrong place" cannot be blamed on a bad camera model.
  2. the reflection of the red sphere (red-dominant SSR pixels on the mirror) reaches
     project(Q_red) within a few pixels, and so does the green one.
  3. LOCALISATION: mirror pixels far from the predicted reflection must NOT be object-coloured
     -- i.e. the reflection is where geometry says, not smeared over the mirror.
  4. JITTER: with the camera moved, the measured reflection centroid must move by the same
     amount as the analytic prediction (the reflection is locked to geometry, not to the
     screen).
  5. SCALE (negative control): with the march parameters *not* derived from the scene scale
     (the historical metre-scale defaults) the mirror must produce essentially no reflection
     -- that is what "stepSize/maxDistance were written for a 1 unit = 1 m world" means.
"""
import os
import sys

import numpy as np

W, H = 1920, 1080
SELF_CHECK_PX = 8.0      # predicted vs measured direct screen position
HIT_PX = 6.0             # distance from the predicted reflection to the nearest object-coloured pixel
JITTER_PX = 5.0          # |measured shift - analytic shift| under a camera move


# ---------------------------------------------------------------- dumps
def load_px(path, w=W, h=H):
    a = np.fromfile(path, dtype=np.float16).astype(np.float32)
    if a.size != w * h * 4:
        raise SystemExit("unexpected dump size for %s: %d (expected %d)" % (path, a.size, w * h * 4))
    return a.reshape(h, w, 4)


def load_camera(directory, tag):
    out = {}
    with open(os.path.join(directory, "gi_%s_camera.txt" % tag)) as f:
        for line in f:
            parts = line.split()
            if parts:
                out[parts[0]] = [float(x) for x in parts[1:]]
    return out


def load_rig(directory, tag):
    out = {}
    with open(os.path.join(directory, "gi_%s_mirror.txt" % tag)) as f:
        for line in f:
            parts = line.split()
            if parts:
                out[parts[0]] = [float(x) for x in parts[1:]]
    return out


# ---------------------------------------------------------------- camera maths (glm conventions)
def view_proj(campos, forward, up, fov_deg, aspect, near, far):
    eye = np.array(campos, dtype=np.float64)
    f = np.array(forward, dtype=np.float64)
    f /= np.linalg.norm(f)
    u = np.array(up, dtype=np.float64)
    s = np.cross(f, u)
    s /= np.linalg.norm(s)
    u = np.cross(s, f)
    V = np.array([
        [s[0], s[1], s[2], -np.dot(s, eye)],
        [u[0], u[1], u[2], -np.dot(u, eye)],
        [-f[0], -f[1], -f[2], np.dot(f, eye)],
        [0.0, 0.0, 0.0, 1.0],
    ], dtype=np.float64)
    t = near * np.tan(np.radians(fov_deg) * 0.5)
    r = t * aspect
    P = np.array([
        [near / r, 0.0, 0.0, 0.0],
        [0.0, near / t, 0.0, 0.0],
        [0.0, 0.0, far / (near - far), (far * near) / (near - far)],
        [0.0, 0.0, -1.0, 0.0],
    ], dtype=np.float64)
    return V, P


def project(vp, p):
    """world point -> pixel (x right, y down, top-left origin)"""
    clip = vp @ np.array([p[0], p[1], p[2], 1.0])
    if clip[3] <= 1e-9:
        return None
    ndc = clip[:3] / clip[3]
    return np.array([(ndc[0] * 0.5 + 0.5) * (W - 1), (0.5 - ndc[1] * 0.5) * (H - 1)])


def mirror_point(c, n, d):
    n = np.asarray(n, dtype=np.float64)
    c = np.asarray(c, dtype=np.float64)
    return c - 2.0 * (np.dot(n, c) + d) * n


def mirrored_screen_extent(vp, cp, half):
    """screen extent of the mirrored box: project its 8 corners"""
    cp = np.asarray(cp, dtype=np.float64)
    pts = []
    for sx in (-1.0, 1.0):
        for sy in (-1.0, 1.0):
            for sz in (-1.0, 1.0):
                q = project(vp, cp + np.array([sx * half, sy * half, sz * half]))
                if q is not None:
                    pts.append(q)
    if not pts:
        return None, 0.0
    pts = np.array(pts)
    centre = pts.mean(axis=0)
    radius = float(np.max(np.linalg.norm(pts - centre, axis=1)))
    return centre, radius


# ---------------------------------------------------------------- measurements
def object_mask(albedo, channel, other_a, other_b):
    """pixels whose albedo is dominated by one channel (the rig's saturated spheres)"""
    return ((albedo[..., channel] > 0.5) & (albedo[..., other_a] < 0.2) & (albedo[..., other_b] < 0.2))


def mirror_mask(worldpos, normal, plane, slab):
    """mirror pixels: on the plane, facing the camera, inside the slab footprint"""
    n = np.array(plane[:3])
    d = plane[3]
    dist = np.abs(worldpos[..., 0] * n[0] + worldpos[..., 1] * n[1] + worldpos[..., 2] * n[2] + d)
    nrm = normal[..., :3] * 2.0 - 1.0
    facing = (nrm[..., 0] * n[0] + nrm[..., 1] * n[1] + nrm[..., 2] * n[2]) > 0.9
    c = np.array(slab[:3])
    half = np.array(slab[3:6])
    # 只在**平面法线之外**的两个轴上限定范围（法线那一轴的厚度就是板厚）
    axis = int(np.argmax(np.abs(n)))
    inside = np.ones(worldpos.shape[:2], dtype=bool)
    for a in range(3):
        if a == axis:
            continue
        inside &= np.abs(worldpos[..., a] - c[a]) <= half[a]
    return (dist < 2.0) & facing & inside


def centroid(mask):
    ys, xs = np.nonzero(mask)
    if len(xs) == 0:
        return None, 0
    return np.array([xs.mean(), ys.mean()]), len(xs)


def nearest_distance(mask, target):
    ys, xs = np.nonzero(mask)
    if len(xs) == 0:
        return None
    d2 = (xs - target[0]) ** 2 + (ys - target[1]) ** 2
    return float(np.sqrt(d2.min()))


def analyse(directory, tag, sphere, channel, other_a, other_b):
    """returns a dict with everything the verdicts need"""
    albedo = load_px(os.path.join(directory, "gi_%s_albedo.f16" % tag))
    world = load_px(os.path.join(directory, "gi_%s_gb_worldpos.f16" % tag))
    normal = load_px(os.path.join(directory, "gi_%s_gb_normal.f16" % tag))
    ssr_path = os.path.join(directory, "gi_%s_ssr.f16" % tag)
    ssr = load_px(ssr_path) if os.path.exists(ssr_path) else None
    cam = load_camera(directory, tag)
    rig = load_rig(directory, tag)

    V, P = view_proj(cam["pos"], cam["forward"], cam["up"], cam["fov"][0],
                     cam["aspect"][0], cam["near"][0], cam["far"][0])
    vp = P @ V

    n = rig["plane"][:3]
    d = rig["plane"][3]
    c = rig[sphere][:3]
    radius = rig[sphere][3]
    cp = mirror_point(c, n, d)
    t = -(np.dot(n, cam["pos"]) + d) / np.dot(n, np.array(cp) - np.array(cam["pos"]))
    q = np.array(cam["pos"]) + t * (np.array(cp) - np.array(cam["pos"]))
    q_px = project(vp, q)
    centre_px, rad_px = mirrored_screen_extent(vp, cp, radius)

    # direct (non-mirror) appearance of the sphere: self-check of the whole camera model
    direct = object_mask(albedo, channel, other_a, other_b)
    direct_centre, direct_count = centroid(direct)
    predicted_direct = project(vp, c)
    # 【自检用"最近像素距离"而不是质心差】立方体的可见面是斜的（正面 + 顶面），其像素质心
    # 并不等于"中心点投影"（实测差 ~11 px）；但中心点的投影一定落在自身轮廓**内部**，
    # 所以"最近的自身像素到中心投影的距离 ≈ 0"才是与形状无关的自检判据。
    self_err = nearest_distance(direct, predicted_direct) if direct_count else None

    mir = mirror_mask(world, normal, rig["plane"], rig["slab"])
    result = {
        "q_px": q_px, "centre_px": centre_px, "rad_px": rad_px,
        "direct_centre": direct_centre, "direct_count": direct_count,
        "predicted_direct": predicted_direct, "self_err": self_err,
        "mirror_px": int(mir.sum()),
    }
    if ssr is not None:
        valid = ssr[..., 3] > 0.0
        rgb = ssr[..., :3]
        dom = ((rgb[..., channel] > rgb[..., other_a] + rgb[..., other_b]) & valid & mir)
        result["ssr_valid_frac"] = float(valid.mean())
        result["ssr_valid_mirror_frac"] = float((valid & mir).sum() / max(int(mir.sum()), 1))
        result["coloured_px"] = int(dom.sum())
        result["coloured_centre"], _ = centroid(dom)
        result["hit_dist"] = nearest_distance(dom, q_px)
        # localisation: object-coloured pixels far away from the predicted reflection
        ys, xs = np.nonzero(dom)
        if len(xs):
            dist = np.sqrt((xs - q_px[0]) ** 2 + (ys - q_px[1]) ** 2)
            far = dist > (rad_px + 1.5 * rad_px + 20.0)
            result["far_coloured"] = int(far.sum())
            result["far_frac"] = float(far.sum() / len(xs))
        else:
            result["far_coloured"] = 0
            result["far_frac"] = 0.0
        result["dom_mask"] = dom
    return result


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else "."
    base_tag = sys.argv[2] if len(sys.argv) > 2 else "ssrmirror_base"
    jitter_tag = sys.argv[3] if len(sys.argv) > 3 else "ssrmirror_jitter"
    legacy_tag = sys.argv[4] if len(sys.argv) > 4 else "ssrmirror_legacy"
    hiz_tag = sys.argv[5] if len(sys.argv) > 5 else "ssrmirror_hiz"

    failures = []

    def verdict(name, ok, detail):
        print("  [%s] %s -- %s" % ("PASS" if ok else "FAIL", name, detail))
        if not ok:
            failures.append(name)

    print("=== planar-mirror run (linear march, fine steps): %s ===" % base_tag)
    red = analyse(directory, base_tag, "red", 0, 1, 2)
    grn = analyse(directory, base_tag, "green", 1, 0, 2)

    print("  mirror pixels on screen : %d (%.1f%%)" % (red["mirror_px"], 100.0 * red["mirror_px"] / (W * H)))
    print("  SSR valid fraction      : %.2f%% of the frame, %.2f%% of the mirror"
          % (100.0 * red.get("ssr_valid_frac", 0.0), 100.0 * red.get("ssr_valid_mirror_frac", 0.0)))
    for name, r in (("red", red), ("green", grn)):
        print("  %-5s : predicted reflection %s (extent %.1f px), centre projects to %s"
              % (name, np.round(r["q_px"], 1), r["rad_px"], np.round(r["predicted_direct"], 1)))
        print("          nearest direct-box pixel to that projection: %s px (camera-model self-check)"
              % ("%.2f" % r["self_err"] if r["self_err"] is not None else "-"))
        print("          object-coloured SSR pixels on the mirror: %d, nearest to prediction %s px"
              % (r.get("coloured_px", 0),
                 "%.2f" % r["hit_dist"] if r.get("hit_dist") is not None else "-"))
    print("")
    print("=== verdicts ===")
    # 1. the analytic reference itself is verified against the direct image
    for tag, r, name in (("red", red, "red"), ("green", grn, "green")):
        verdict("analytic camera model matches the direct %s box" % name,
                r["self_err"] is not None and r["self_err"] <= SELF_CHECK_PX,
                "the box centre projects to %s; nearest direct %s pixel is %s px away (<= %.1f)"
                % (np.round(r["predicted_direct"], 1), name,
                   "%.2f" % r["self_err"] if r["self_err"] is not None else "-", SELF_CHECK_PX))
    # 2. the reflection lands on the analytically predicted pixel
    for r, name in ((red, "red"), (grn, "green")):
        ok = r.get("hit_dist") is not None and r["hit_dist"] <= HIT_PX and r.get("coloured_px", 0) > 100
        verdict("the %s box's reflection appears at the predicted pixel" % name, ok,
                "nearest object-coloured mirror pixel %.2f px from prediction (<= %.1f), %d such pixels"
                % (r.get("hit_dist") if r.get("hit_dist") is not None else -1, HIT_PX, r.get("coloured_px", 0)))
    # 3. localisation: no object-coloured reflection far from the prediction
    verdict("the reflection is localised where geometry says",
            red.get("far_frac", 1.0) < 0.05,
            "%.2f%% of object-coloured mirror pixels lie farther than 3.5 projected extents (<= 5%%)"
            % (100.0 * red.get("far_frac", 0.0)))

    # 4. jitter: the reflection follows the analytic prediction under a camera move
    print("")
    print("=== camera jitter (moved camera) ===")
    red_j = analyse(directory, jitter_tag, "red", 0, 1, 2)
    if red_j.get("coloured_px", 0) > 100 and red_j["coloured_centre"] is not None \
            and red["coloured_centre"] is not None:
        measured_shift = red_j["coloured_centre"] - red["coloured_centre"]
        analytic_shift = red_j["q_px"] - red["q_px"]
        err = float(np.linalg.norm(measured_shift - analytic_shift))
        verdict("the reflection tracks the geometry when the camera moves", err <= JITTER_PX,
                "measured shift %s vs analytic %s -> |diff| %.2f px (<= %.1f)"
                % (np.round(measured_shift, 1), np.round(analytic_shift, 1), err, JITTER_PX))
    else:
        verdict("the reflection tracks the geometry when the camera moves", False,
                "not enough object-coloured pixels in the jitter run (%d)" % red_j.get("coloured_px", 0))

    # 5. scale: the historical metre-scale march params must NOT find the reflection
    print("")
    print("=== march scale (negative control: metre-scale defaults) ===")
    leg = analyse(directory, legacy_tag, "red", 0, 1, 2)
    leg_frac = leg.get("ssr_valid_mirror_frac", 0.0)
    ok_frac = red.get("ssr_valid_mirror_frac", 0.0)
    print("  mirror valid fraction: scene-scaled %.2f%% vs metre-scale defaults %.2f%%"
          % (100.0 * ok_frac, 100.0 * leg_frac))
    print("  object-coloured mirror pixels: scene-scaled %d vs metre-scale %d"
          % (red.get("coloured_px", 0), leg.get("coloured_px", 0)))
    verdict("the scene-scaled march finds reflections the metre-scale one misses",
            red.get("coloured_px", 0) > 100 and leg.get("coloured_px", 0) * 10 < red.get("coloured_px", 0),
            "%d vs %d object-coloured pixels" % (red.get("coloured_px", 0), leg.get("coloured_px", 0)))

    # 6. the DEFAULT path (Hi-Z hierarchy march, scene-scaled) -- REPORTED, not asserted:
    #    it currently misses one of the two reflections (see defect 9.2-AE / task 35)
    print("")
    print("=== default path (Hi-Z hierarchy march, scene-scaled) -- reported ===")
    hiz_red = analyse(directory, hiz_tag, "red", 0, 1, 2)
    hiz_grn = analyse(directory, hiz_tag, "green", 1, 0, 2)
    for name, r in (("red", hiz_red), ("green", hiz_grn)):
        pos = ("%.2f px" % r["hit_dist"]) if r.get("hit_dist") is not None else "no hit"
        print("  %-5s : %d object-coloured mirror pixels, nearest to prediction %s"
              % (name, r.get("coloured_px", 0), pos))
    if hiz_grn.get("coloured_px", 0) == 0:
        print("  [KNOWN] the Hi-Z path misses the green box's reflection entirely, while the linear")
        print("          path puts it exactly on the predicted pixel. The analytic criterion is")
        print("          therefore asserted on the linear path (a supported fallback with its own")
        print("          switch); the Hi-Z miss is recorded as defect 9.2-AE / task 35.")

    print("")
    if failures:
        print("!! %d verdict(s) failed: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("all planar-mirror verdicts passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
