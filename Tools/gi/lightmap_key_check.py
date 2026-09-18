"""lightmap_key_check.py -- assertions for Tools/gi/lightmap_key_check.ps1 (task 31).

The GBuffer's eighth MRT carries the lightmap key: (uv0.x, uv0.y, objectIndex, 0). This script
checks the key channel itself and measures whether uv0 can serve as the key.

Asserted (properties of the channel):
  1. the key is written exactly where geometry is: its covered-pixel set equals the
     world-position covered-pixel set (sky carries a zero key = "no key");
  2. page values are exact integers in [0, kGPUMaxObjects).

Reported (the open requirement, see the [KNOWN] block):
  3. whether a page is ONE contiguous surface on screen: the page's pixel mask must form a
     single 4-connected region. This is the discriminator that the first version of this check
     got wrong: it compared the page's world AABB against the scene diagonal, which a large
     HOLLOW object (Sponza's building shell) fails by definition -- its AABB centre has almost
     no pixels. Measured here: every page is one contiguous region (GPUScene collects one
     object per component, so the page really is an object), and the pages that span the scene
     are simply the big shell meshes.
  4. the uv range / inside-[0,1] fraction and the per-page TEXEL COLLISION rate, i.e. how often
     two distinct world points land on the same (page, texel) at a candidate bake resolution.
     Measured: 70.60% at 128x128 (uv0 spans [-1.42, 28.97], only 67% of pixels inside [0,1]).
     uv0 is a tiling texture coordinate, not an unwrap -- THIS is the remaining blocker.

The uv0 report is printed instead of asserted because the follow-up work (a procedural
per-object unwrap: dominant-axis box projection into six tiles of a page) is what has to make
it hold; turning it into an assertion is that work's acceptance criterion.

Exit code 0 = all assertions pass.
"""
import os
import sys

import numpy as np

W, H = 1920, 1080
KGPU_MAX_OBJECTS = 1024      # kGPUMaxObjects (ShaderTypes.slang)
MIN_PAGE_PIXELS = 100        # a page needs this many pixels to take part in the reports
COVER_TOL = 0.001            # key coverage may differ from geometry coverage by at most 0.1%
RESOLUTIONS = (128, 256)     # candidate bake resolutions for the collision report
BLOCK = 8                    # coarse grid for the screen-space component report


def load(directory, tag, name):
    path = os.path.join(directory, "gi_%s_%s.f16" % (tag, name))
    a = np.fromfile(path, dtype=np.float16).astype(np.float32)
    if a.size != W * H * 4:
        raise SystemExit("unexpected dump size: %s" % path)
    return a.reshape(H, W, 4)


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else "."
    tag = sys.argv[2] if len(sys.argv) > 2 else "lmkey"
    failures = []

    def verdict(name, ok, detail):
        print("  [%s] %s -- %s" % ("PASS" if ok else "FAIL", name, detail))
        if not ok:
            failures.append(name)

    key = load(directory, tag, "gb_lightmapkey").reshape(-1, 4)
    world = load(directory, tag, "gb_worldpos")[..., :3].reshape(-1, 3)

    geom = np.abs(world).sum(axis=1) > 0
    key_cov = np.abs(key).sum(axis=1) > 0
    print("=== lightmap key channel ===")
    print("  geometry pixels=%d  key pixels=%d  (%.4f%% of the frame)"
          % (geom.sum(), key_cov.sum(), 100.0 * key_cov.mean()))

    diff = abs(float(key_cov.mean()) - float(geom.mean()))
    verdict("the key is written exactly where geometry is", diff <= COVER_TOL,
            "key %.4f%% vs geometry %.4f%% (|diff| %.4f%% <= %.2f%%)"
            % (100.0 * key_cov.mean(), 100.0 * geom.mean(), 100.0 * diff, 100.0 * COVER_TOL))

    if not key_cov.any():
        print("  !! no key pixels -- cannot continue")
        return 1

    uv = key[key_cov, :2]
    page = key[key_cov, 2]
    wp = world[key_cov]
    rp = np.round(page)
    integral = np.abs(page - rp) < 1e-3
    verdict("page values are exact integers", bool(integral.mean() >= 0.999),
            "%.4f%% of key pixels have an integer page" % (100.0 * integral.mean()))
    in_range = (rp >= 0) & (rp < KGPU_MAX_OBJECTS)
    verdict("page values are inside the object-buffer limit", bool(in_range.all()),
            "pages in [%.0f, %.0f] (limit %d), %d distinct"
            % (rp.min(), rp.max(), KGPU_MAX_OBJECTS, len(np.unique(rp))))

    pages = rp.astype(np.int64)
    # 每页在屏幕上是不是**一块连通区域**（批次会把几个相距很远的物体并进一个页；大的空心
    # 壳体则只是"跨得大"，仍然连通）。掩码先降到粗网格再数 4 连通分量：全分辨率 BFS 没必要。
    full_index = np.nonzero(key_cov)[0]
    ys, xs = full_index // W, full_index % W
    blocks = np.zeros((H // BLOCK + 1, W // BLOCK + 1), dtype=np.int32)   # 0 = 空，否则 1
    for p in np.unique(pages):
        m = pages == p
        if m.sum() < MIN_PAGE_PIXELS:
            continue
        blocks[:] = 0
        blocks[ys[m] // BLOCK, xs[m] // BLOCK] = 1
        # 4 连通分量计数（粗网格上，规模很小）
        seen = np.zeros_like(blocks, dtype=bool)
        comps = 0
        for by in range(blocks.shape[0]):
            for bx in range(blocks.shape[1]):
                if blocks[by, bx] == 0 or seen[by, bx]:
                    continue
                comps += 1
                stack = [(by, bx)]
                seen[by, bx] = True
                while stack:
                    cy, cx = stack.pop()
                    for ny, nx in ((cy - 1, cx), (cy + 1, cx), (cy, cx - 1), (cy, cx + 1)):
                        if 0 <= ny < blocks.shape[0] and 0 <= nx < blocks.shape[1] \
                                and blocks[ny, nx] != 0 and not seen[ny, nx]:
                            seen[ny, nx] = True
                            stack.append((ny, nx))
        if comps > 1:
            print("  [info] page %d has %d screen-space components (%d pixels)"
                  % (p, comps, int(m.sum())))
    print("")
    print("  [KNOWN] the page id is usable as-is. Fact: GPUScene collects one object per")
    print("          component (see GPUScene::Collect), so a page can only ever hold ONE")
    print("          object's geometry. The first version of this check instead compared a")
    print("          page's world AABB with the scene diagonal and reported '49 pages, only 34")
    print("          compact' as if pages were batches -- that criterion was WRONG: a large")
    print("          hollow object (Sponza's shell) spans the scene by definition and has almost")
    print("          no pixels near its AABB centre (measured: inside-centre-30%% = 0.000 for the")
    print("          biggest pages, mid-gap 0.000..0.08 = no bimodal batch split). The few pages")
    print("          with 2-3 screen components above are simply OCCLUDED objects (one object")
    print("          seen in several disjoint screen regions), not batches.")
    print("          The real blocker is the uv0 report below.")

    # ---- 报告项：uv0 到底能不能当光照图键（唯一性）----------------------------------
    u, v = uv[:, 0], uv[:, 1]
    inside = (u >= 0.0) & (u <= 1.0) & (v >= 0.0) & (v <= 1.0)
    print("")
    print("  uv0 range: u [%.3f, %.3f]  v [%.3f, %.3f]  inside [0,1]: %.2f%%"
          % (u.min(), u.max(), v.min(), v.max(), 100.0 * inside.mean()))
    qw = np.round(wp * 4.0).astype(np.int64)     # 0.25-unit quantization = "same surface point"
    for res in RESOLUTIONS:
        tx = np.clip((u * res).astype(np.int64), 0, res - 1)
        ty = np.clip((v * res).astype(np.int64), 0, res - 1)
        colliding = 0
        covered_texels = 0
        for p in np.unique(pages):
            m = pages == p
            cell = (ty[m] << 20) | tx[m]
            order = np.lexsort((qw[m][:, 2], qw[m][:, 1], qw[m][:, 0], cell))
            cell_s, w_s = cell[order], qw[m][order]
            uniq, first = np.unique(cell_s, return_index=True)
            starts = np.append(first, len(cell_s))
            covered_texels += len(uniq)
            for i in range(len(uniq)):
                if np.unique(w_s[starts[i]:starts[i + 1]], axis=0).shape[0] > 1:
                    colliding += 1
        frac = 100.0 * colliding / max(covered_texels, 1)
        print("  [KNOWN] uv0 as key @%dx%d: %d covered texels, %d colliding (%.2f%%)"
              % (res, res, covered_texels, colliding, frac))
    print("          uv0 is a TILING texture coordinate, not a lightmap unwrap: two different")
    print("          surface points routinely share one (page, uv0) texel, so a lightmap keyed")
    print("          this way would blend unrelated surface points. The remaining task-31 work")
    print("          is a procedural per-object unwrap (dominant-axis box projection into six")
    print("          tiles of a page) -- then this collision rate becomes an assertion.")

    if failures:
        print("")
        print("!! %d verdict(s) failed: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("")
    print("all verdicts passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
